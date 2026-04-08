#define RSPCLIENT_STATIC

#include "client/cpp/rsp_client.hpp"
#include "client/cpp/rsp_client_message.hpp"
#include "client/cpp_full/rsp_client.hpp"
#include "common/endorsement/endorsement.hpp"
#include "common/endorsement/well_known_endorsements.h"
#include "endorsement_service/endorsement_service.hpp"

#include "common/ascii_handshake.hpp"
#include "common/transport/transport_memory.hpp"
#include "resource_manager/resource_manager.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
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
    const auto connectionId = es->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

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

    const auto esConnectionId = es->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

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

    const auto esConnectionId = es->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

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

        const auto esConnectionId = es->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

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

    const auto esConnectionId = es->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

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
    } catch (const std::exception& ex) {
        std::cerr << "FAILED: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
