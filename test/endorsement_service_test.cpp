#include "client/cpp/rsp_client.hpp"
#include "client/cpp/rsp_client_message.hpp"
#include "client/cpp_full/rsp_client.hpp"
#include "common/endorsement/endorsement.hpp"
#include "common/endorsement/well_known_endorsements.h"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "endorsement_service/endorsement_service.hpp"

#include "common/transport/transport_memory.hpp"
#include "resource_manager/resource_manager.hpp"

#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class TestResourceManager : public rsp::resource_manager::ResourceManager {
public:
    using rsp::resource_manager::ResourceManager::ResourceManager;

    rsp::NodeID nodeId() const {
        return keyPair().nodeID();
    }
};

class SignerRestrictedResourceManager : public TestResourceManager {
public:
    SignerRestrictedResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports,
                                    rsp::NodeID allowedSigner,
                                                                        std::map<rsp::NodeID, std::shared_ptr<rsp::KeyPair>> endorsementServiceKeys)
        : TestResourceManager(std::move(clientTransports)),
          allowedSigner_(allowedSigner),
          endorsementServiceKeys_(std::move(endorsementServiceKeys)) {
                rebuildAuthorizationQueue();
    }

protected:
    std::vector<rsp::Endorsement> getAuthorizationEndorsements(const rsp::NodeID& nodeId) const override {
        const auto iterator = endorsementServiceKeys_.find(nodeId);
        if (iterator == endorsementServiceKeys_.end()) {
            return {};
        }

        rsp::DateTime validUntil;
        validUntil += HOURS(1);

        return {rsp::Endorsement::createSigned(
            *iterator->second,
            nodeId,
            rsp::GUID("00112233-4455-6677-8899-aabbccddeeff"),
            rsp::Buffer(),
            validUntil)};
    }

    rsp::proto::ERDAbstractSyntaxTree authorizationTree() const override {
        rsp::proto::ERDAbstractSyntaxTree tree;
        std::string value;
        value.reserve(16);
        for (int shift = 56; shift >= 0; shift -= 8) {
            value.push_back(static_cast<char>((allowedSigner_.high() >> shift) & 0xFFULL));
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            value.push_back(static_cast<char>((allowedSigner_.low() >> shift) & 0xFFULL));
        }
        tree.mutable_message_source()->mutable_source()->set_value(value);
        return tree;
    }

private:
    rsp::NodeID allowedSigner_;
    std::map<rsp::NodeID, std::shared_ptr<rsp::KeyPair>> endorsementServiceKeys_;
};

class TestEndorsementService : public rsp::endorsement_service::EndorsementService {
public:
    using Ptr = std::shared_ptr<TestEndorsementService>;

    static Ptr create(rsp::KeyPair keyPair) {
        return Ptr(new TestEndorsementService(std::move(keyPair)));
    }

    bool tryDequeueHandledMessage(rsp::proto::RSPMessage& message) {
        std::lock_guard<std::mutex> lock(handledMessagesMutex_);
        if (handledMessages_.empty()) {
            return false;
        }

        message = std::move(handledMessages_.front());
        handledMessages_.pop_front();
        return true;
    }

    size_t pendingHandledMessageCount() const {
        std::lock_guard<std::mutex> lock(handledMessagesMutex_);
        return handledMessages_.size();
    }

protected:
    explicit TestEndorsementService(rsp::KeyPair keyPair)
        : rsp::endorsement_service::EndorsementService(std::move(keyPair)) {
    }

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override {
        if (message.has_ping_reply() || message.has_endorsement_needed() || message.has_error()) {
            std::lock_guard<std::mutex> lock(handledMessagesMutex_);
            handledMessages_.push_back(message);
            return true;
        }

        return rsp::endorsement_service::EndorsementService::handleNodeSpecificMessage(message);
    }

private:
    mutable std::mutex handledMessagesMutex_;
    std::deque<rsp::proto::RSPMessage> handledMessages_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool waitForCondition(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

bool parseEndorsementMessage(const rsp::Endorsement& endorsement, rsp::proto::Endorsement& message) {
    const rsp::Buffer serialized = endorsement.serialize();
    return message.ParseFromArray(serialized.data(), static_cast<int>(serialized.size()));
}

rsp::Buffer stringToBuffer(const std::string& value) {
    if (value.empty()) {
        return rsp::Buffer();
    }

    return rsp::Buffer(reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size()));
}

std::string bufferToString(const rsp::Buffer& value) {
    if (value.empty()) {
        return std::string();
    }

    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoNodeId.set_value(value);
    return protoNodeId;
}

rsp::Endorsement parseEndorsement(const rsp::proto::Endorsement& message) {
    std::string serialized;
    if (!message.SerializeToString(&serialized)) {
        throw std::runtime_error("failed to serialize endorsement message in test");
    }

    return rsp::Endorsement::deserialize(stringToBuffer(serialized));
}

void testEndorsementServiceConnectsToResourceManager() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair esKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID esNodeId = esKeyPair.nodeID();

    std::promise<rsp::encoding::EncodingHandle> handshakePromise;
    std::future<rsp::encoding::EncodingHandle> handshakeFuture = handshakePromise.get_future();
    resourceManager.setNewEncodingCallback([&handshakePromise](const rsp::encoding::EncodingHandle& encoding) {
        try {
            handshakePromise.set_value(encoding);
        } catch (...) {
            handshakePromise.set_exception(std::current_exception());
        }
    });

    serverTransport->listen("rm-test");
    const std::string transportSpec = "memory:rm-test";

    auto es = rsp::endorsement_service::EndorsementService::create(std::move(esKeyPair));
    const auto connectionId = es->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

    require(es->hasConnections(), "endorsement service should track created connections");
    require(es->hasConnection(connectionId), "endorsement service should expose the new connection id");
    require(es->connectionCount() == 1, "endorsement service should report one live connection");
    require(es->connectionIds().size() == 1, "endorsement service should enumerate its single live connection");

    require(handshakeFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "resource manager handshake pipeline should complete for endorsement service");
    const rsp::encoding::EncodingHandle serverEncoding = handshakeFuture.get();
    require(serverEncoding != nullptr, "resource manager should activate an encoding for the endorsement service");
    require(resourceManager.activeEncodingCount() == 1,
            "resource manager should expose one authenticated encoding");

    const auto esPeerNodeId = es->peerNodeID(connectionId);
    require(esPeerNodeId.has_value(),
            "endorsement service should learn the resource manager node id during authentication");
    require(esPeerNodeId.value() == resourceManager.nodeId(),
            "endorsement service should store the resource manager node id");

    const auto serverPeerNodeId = serverEncoding->peerNodeID();
    require(serverPeerNodeId.has_value(),
            "resource manager encoding should learn the endorsement service node id during authentication");
    require(serverPeerNodeId.value() == esNodeId,
            "resource manager encoding should store the endorsement service node id");

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 1; }),
            "resource manager should keep the endorsement service connection active");
    require(resourceManager.pendingMessageCount() == 0,
            "resource manager should not expose authentication traffic through its incoming message queue");

    require(es->removeConnection(connectionId), "endorsement service should remove an existing connection");
    require(!es->hasConnections(), "endorsement service should have no remaining connections");

    serverTransport->stop();
}

void testClientPingsEndorsementService() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair esKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID esNodeId = esKeyPair.nodeID();

    serverTransport->listen("rm-test");
    const std::string transportSpec = "memory:rm-test";

    auto es = rsp::endorsement_service::EndorsementService::create(std::move(esKeyPair));
    auto client = rsp::client::RSPClient::create();

    const auto esConnectionId = es->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

    require(es->hasConnection(esConnectionId),
            "endorsement service should stay connected to the resource manager");
    require(client->hasConnection(clientConnectionId),
            "client should stay connected to the resource manager");
    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both the client and endorsement service");

    require(client->ping(esNodeId),
            "client should ping the endorsement service through the resource manager");

    require(es->removeConnection(esConnectionId),
            "endorsement service should remove its resource manager connection");
    require(client->removeConnection(clientConnectionId),
            "client should remove its resource manager connection");

    serverTransport->stop();
}

void testClientRequestsNetworkAccessEndorsement() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair esKeyPair = rsp::KeyPair::generateP256();
    rsp::KeyPair esVerifyKey = esKeyPair.duplicate();
    const rsp::NodeID esNodeId = esKeyPair.nodeID();

    rsp::KeyPair clientKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID clientNodeId = clientKeyPair.nodeID();

    serverTransport->listen("rm-test");
    const std::string transportSpec = "memory:rm-test";

    auto es = rsp::endorsement_service::EndorsementService::create(std::move(esKeyPair));
    auto client = rsp::client::RSPClient::create(std::move(clientKeyPair));

    const auto esConnectionId = es->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both the client and endorsement service before endorsement requests");

    const rsp::DateTime requestStart;
    const auto reply = client->beginEndorsementRequest(
        esNodeId,
        ETYPE_ACCESS,
        stringToBuffer(EVALUE_ACCESS_NETWORK.toString()));
    require(reply.has_value(), "client should receive an endorsement response from the endorsement service");
    require(reply->status() == rsp::proto::ENDORSEMENT_SUCCESS,
            "endorsement service should accept a valid network access endorsement request");
    require(reply->has_new_endorsement(), "successful endorsement responses should include a signed endorsement");

    const rsp::Endorsement issuedEndorsement = parseEndorsement(reply->new_endorsement());
    require(issuedEndorsement.subject() == clientNodeId,
            "issued endorsement should target the requesting client");
    require(issuedEndorsement.endorsementService() == esNodeId,
            "issued endorsement should identify the endorsement service as signer");
    require(issuedEndorsement.endorsementType() == ETYPE_ACCESS,
            "issued endorsement should preserve the requested endorsement type");
    require(bufferToString(issuedEndorsement.endorsementValue()) == EVALUE_ACCESS_NETWORK.toString(),
            "issued endorsement should preserve the requested network access value");
    require(issuedEndorsement.verifySignature(esVerifyKey),
            "issued endorsement should verify against the endorsement service key");

    const double lifetimeSeconds = issuedEndorsement.validUntil().secondsSinceEpoch() - requestStart.secondsSinceEpoch();
    require(lifetimeSeconds >= DAYS(1) - 5.0 && lifetimeSeconds <= DAYS(1) + 5.0,
            "issued endorsement should be valid for approximately one day from issuance");

    require(es->removeConnection(esConnectionId),
            "endorsement service should remove its resource manager connection after the endorsement test");
    require(client->removeConnection(clientConnectionId),
            "client should remove its resource manager connection after the endorsement test");

    serverTransport->stop();
}

    void testBeginEndorsementRequestWithoutIdentityReturnsUnknownIdentity() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair esKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID esNodeId = esKeyPair.nodeID();

        rsp::KeyPair clientKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID clientNodeId = clientKeyPair.nodeID();
        rsp::KeyPair clientSigningKey = clientKeyPair.duplicate();

        serverTransport->listen("rm-test");
    const std::string transportSpec = "memory:rm-test";

        auto es = rsp::endorsement_service::EndorsementService::create(std::move(esKeyPair));
        auto client = rsp::client::RSPClientMessage::create(std::move(clientKeyPair));

        const auto esConnectionId = es->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both participants before raw endorsement requests");

        rsp::DateTime requestedValidUntil;
        requestedValidUntil += DAYS(1);
        const rsp::Endorsement requested = rsp::Endorsement::createSigned(
            clientSigningKey,
        clientNodeId,
        ETYPE_ACCESS,
        stringToBuffer(EVALUE_ACCESS_NETWORK.toString()),
        requestedValidUntil);

        rsp::proto::Endorsement requestedMessage;
        require(parseEndorsementMessage(requested, requestedMessage),
            "test endorsement request should parse into the protobuf endorsement message");

        rsp::proto::RSPMessage request;
        *request.mutable_source() = toProtoNodeId(clientNodeId);
        *request.mutable_destination() = toProtoNodeId(esNodeId);
        *request.mutable_begin_endorsement_request()->mutable_requested_values() = requestedMessage;

        require(client->send(request), "raw client should send the unsigned-identity endorsement request");
        require(waitForCondition([&client]() { return client->pendingMessageCount() == 1; }),
            "raw client should receive an endorsement response");

        rsp::proto::RSPMessage reply;
        require(client->tryDequeueMessage(reply), "raw client should expose the endorsement reply");
        require(reply.has_endorsement_done(), "endorsement service should respond with an endorsement result");
        require(reply.endorsement_done().status() == rsp::proto::ENDORSEMENT_UNKNOWN_IDENTITY,
            "endorsement service should reject requests whose identity is not yet cached");
        require(!es->identityCache().contains(clientNodeId),
            "endorsement service should not populate the identity cache from a begin request alone");

        require(es->removeConnection(esConnectionId),
            "endorsement service should remove its resource manager connection after raw endorsement failure test");
        require(client->removeConnection(clientConnectionId),
            "raw client should remove its resource manager connection after raw endorsement failure test");

        serverTransport->stop();
    }

void testForwardedIdentityMessagesPopulateResourceManagerAndEndorsementServiceCaches() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair esKeyPair = rsp::KeyPair::generateP256();
    rsp::KeyPair clientKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID esNodeId = esKeyPair.nodeID();
    const rsp::NodeID clientNodeId = clientKeyPair.nodeID();
    const rsp::proto::PublicKey clientPublicKey = clientKeyPair.publicKey();

    serverTransport->listen("rm-test");
    const std::string transportSpec = "memory:rm-test";

    auto es = rsp::endorsement_service::EndorsementService::create(std::move(esKeyPair));
    auto client = rsp::client::full::RSPClient::create(std::move(clientKeyPair));

    const auto esConnectionId = es->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both the client and endorsement service before forwarding identities");

    rsp::proto::RSPMessage identityMessage;
    *identityMessage.mutable_source() = toProtoNodeId(clientNodeId);
    *identityMessage.mutable_destination() = toProtoNodeId(esNodeId);
    *identityMessage.mutable_identity()->mutable_public_key() = clientPublicKey;

    require(client->send(identityMessage),
            "full client should be able to send an identity message through the resource manager");
    require(waitForCondition([&resourceManager, &clientNodeId]() {
                return resourceManager.identityCache().contains(clientNodeId);
            }),
            "resource manager should cache forwarded identity messages");
    require(waitForCondition([&es, &clientNodeId]() {
                return es->identityCache().contains(clientNodeId);
            }),
            "endorsement service should cache forwarded identity messages addressed to it");

    require(es->removeConnection(esConnectionId),
            "endorsement service should remove its resource manager connection after forwarded identity test");
    require(client->removeConnection(clientConnectionId),
            "full client should remove its resource manager connection after forwarded identity test");

    serverTransport->stop();
}

    void testMessageSourceAuthorizationAllowsFirstEndorsementServiceOnly() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();

        rsp::KeyPair firstEsKeyPair = rsp::KeyPair::generateP256();
        rsp::KeyPair secondEsKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID firstEsNodeId = firstEsKeyPair.nodeID();
        const rsp::NodeID secondEsNodeId = secondEsKeyPair.nodeID();

        SignerRestrictedResourceManager resourceManager(
        {serverTransport},
        firstEsNodeId,
            {{firstEsNodeId, std::make_shared<rsp::KeyPair>(firstEsKeyPair.duplicate())},
             {secondEsNodeId, std::make_shared<rsp::KeyPair>(secondEsKeyPair.duplicate())}});

        serverTransport->listen("rm-signer-authz-test");
        const std::string transportSpec = "memory:rm-signer-authz-test";

        auto firstEs = TestEndorsementService::create(std::move(firstEsKeyPair));
        auto secondEs = TestEndorsementService::create(std::move(secondEsKeyPair));

        const auto firstConnectionId =
        firstEs->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto secondConnectionId =
        secondEs->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endorsement services before authz checks");

        rsp::proto::RSPMessage firstPing;
        *firstPing.mutable_destination() = toProtoNodeId(resourceManager.nodeId());
        firstPing.mutable_ping_request()->mutable_nonce()->set_value("es0-ping");
        firstPing.mutable_ping_request()->set_sequence(1);
        firstPing.mutable_ping_request()->mutable_time_sent()->set_milliseconds_since_epoch(111);

        rsp::proto::RSPMessage secondPing;
        *secondPing.mutable_destination() = toProtoNodeId(resourceManager.nodeId());
        secondPing.mutable_ping_request()->mutable_nonce()->set_value("es1-ping");
        secondPing.mutable_ping_request()->set_sequence(2);
        secondPing.mutable_ping_request()->mutable_time_sent()->set_milliseconds_since_epoch(222);

        require(firstEs->send(firstPing), "first endorsement service should send its ping through RM");
        require(secondEs->send(secondPing), "second endorsement service should send its ping through RM");

        require(waitForCondition([&firstEs]() { return firstEs->pendingHandledMessageCount() == 1; }),
            "authorized endorsement service should receive a ping reply");
        require(waitForCondition([&secondEs]() { return secondEs->pendingHandledMessageCount() == 1; }),
            "unauthorized endorsement service should receive an authz failure reply");

        rsp::proto::RSPMessage firstReply;
        require(firstEs->tryDequeueHandledMessage(firstReply),
            "first endorsement service should expose its handled reply");
        require(firstReply.has_ping_reply(),
            "message_source authorization should allow the first endorsement service to ping the RM");
        require(firstReply.ping_reply().nonce().value() == firstPing.ping_request().nonce().value(),
            "authorized ping reply should preserve the request nonce");

        rsp::proto::RSPMessage secondReply;
        require(secondEs->tryDequeueHandledMessage(secondReply),
            "second endorsement service should expose its handled reply");
        require(secondReply.has_endorsement_needed(),
            "message_source authorization should reject the second endorsement service");
        require(secondReply.endorsement_needed().has_tree(),
            "authorization failure should include the required endorsement tree");
        require(secondReply.endorsement_needed().tree().has_message_source(),
            "authorization failure tree should require message_source");
        std::string expectedSignerValue;
        expectedSignerValue.reserve(16);
        for (int shift = 56; shift >= 0; shift -= 8) {
            expectedSignerValue.push_back(static_cast<char>((firstEsNodeId.high() >> shift) & 0xFFULL));
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            expectedSignerValue.push_back(static_cast<char>((firstEsNodeId.low() >> shift) & 0xFFULL));
        }
        require(secondReply.endorsement_needed().tree().message_source().source().value() ==
            expectedSignerValue,
            "authorization failure tree should name the first endorsement service as the allowed signer");
        require(secondReply.endorsement_needed().has_message_nonce(),
            "authorization failure should include the rejected message nonce");
        require(secondReply.endorsement_needed().message_nonce().value() == secondReply.nonce().value(),
            "authorization failure should mirror the rejected message nonce into EndorsementNeeded");

        require(firstEs->removeConnection(firstConnectionId),
            "first endorsement service should remove its resource manager connection");
        require(secondEs->removeConnection(secondConnectionId),
            "second endorsement service should remove its resource manager connection");

        serverTransport->stop();
    }

}  // namespace

int main() {
    try {
        testEndorsementServiceConnectsToResourceManager();
        std::cout << "testEndorsementServiceConnectsToResourceManager: PASSED\n";

        testClientPingsEndorsementService();
        std::cout << "testClientPingsEndorsementService: PASSED\n";

        testClientRequestsNetworkAccessEndorsement();
        std::cout << "testClientRequestsNetworkAccessEndorsement: PASSED\n";

        testBeginEndorsementRequestWithoutIdentityReturnsUnknownIdentity();
        std::cout << "testBeginEndorsementRequestWithoutIdentityReturnsUnknownIdentity: PASSED\n";

        testForwardedIdentityMessagesPopulateResourceManagerAndEndorsementServiceCaches();
        std::cout << "testForwardedIdentityMessagesPopulateResourceManagerAndEndorsementServiceCaches: PASSED\n";

        testMessageSourceAuthorizationAllowsFirstEndorsementServiceOnly();
        std::cout << "testMessageSourceAuthorizationAllowsFirstEndorsementServiceOnly: PASSED\n";
    } catch (const std::exception& ex) {
        std::cerr << "FAILED: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
