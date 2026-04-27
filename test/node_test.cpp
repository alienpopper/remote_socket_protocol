#include "common/node.hpp"
#include "common/message_queue/mq_signing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class TestNode : public rsp::RSPNode {
public:
    ~TestNode() override {
        stopNodeQueues();
    }

    int run() const override {
        return 0;
    }

protected:
    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override {
        if (message.has_identity()) {
            return true;
        }

        return false;
    }

    void handleOutputMessage(rsp::proto::RSPMessage) override {
    }

public:

    void pauseOutputQueue() {
        outputQueue()->pause();
    }

    rsp::NodeID nodeId() const {
        return keyPair().nodeID();
    }

    bool tryPopOutput(rsp::proto::RSPMessage& message) {
        return outputQueue()->tryPop(message);
    }

    std::optional<rsp::Buffer> aesKeyMaterial(const rsp::NodeID& peerNodeId) const {
        return aesKeyMaterialForPeer(peerNodeId);
    }
};

class CapturingNode : public rsp::RSPNode {
public:
    ~CapturingNode() override {
        stopNodeQueues();
    }

    int run() const override {
        return 0;
    }

    rsp::NodeID nodeId() const {
        return keyPair().nodeID();
    }

    bool tryPopCapturedOutput(rsp::proto::RSPMessage& message) {
        std::lock_guard<std::mutex> lock(capturedMutex_);
        if (capturedOutputs_.empty()) {
            return false;
        }
        message = std::move(capturedOutputs_.front());
        capturedOutputs_.pop_front();
        return true;
    }

    size_t capturedOutputCount() const {
        std::lock_guard<std::mutex> lock(capturedMutex_);
        return capturedOutputs_.size();
    }

    std::optional<rsp::Buffer> aesKeyMaterial(const rsp::NodeID& peerNodeId) const {
        return aesKeyMaterialForPeer(peerNodeId);
    }

protected:
    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override {
        if (message.has_identity()) {
            return true;
        }
        return false;
    }

    void handleOutputMessage(rsp::proto::RSPMessage message) override {
        std::lock_guard<std::mutex> lock(capturedMutex_);
        capturedOutputs_.push_back(std::move(message));
    }

private:
    mutable std::mutex capturedMutex_;
    std::deque<rsp::proto::RSPMessage> capturedOutputs_;
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

rsp::proto::RSPMessage makeIdentityMessage(const rsp::KeyPair& keyPair) {
    rsp::proto::RSPMessage message;
    *message.mutable_source() = toProtoNodeId(keyPair.nodeID());
    message.mutable_identity()->mutable_nonce()->set_value("identity-nonce");
    *message.mutable_identity()->mutable_public_key() = keyPair.publicKey();
    return message;
}

rsp::proto::RSPMessage makePingRequest() {
    rsp::proto::RSPMessage message;
    message.mutable_source()->set_value("requester-node");
    message.mutable_ping_request()->mutable_nonce()->set_value("ping-nonce");
    message.mutable_ping_request()->set_sequence(7);
    message.mutable_ping_request()->mutable_time_sent()->set_milliseconds_since_epoch(123456);
    return message;
}

rsp::proto::RSPMessage makeUnsupportedMessage() {
    rsp::proto::RSPMessage message;
    message.mutable_source()->set_value("requester-node");
    message.mutable_route();
    return message;
}

rsp::proto::RSPMessage makeChallengeRequest(const rsp::NodeID& requesterNodeId, const std::string& nonce) {
    rsp::proto::RSPMessage message;
    *message.mutable_source() = toProtoNodeId(requesterNodeId);
    message.mutable_challenge_request()->mutable_nonce()->set_value(nonce);
    return message;
}

rsp::proto::RSPMessage makeAesKeyNegotiationRequest(const rsp::NodeID& requesterNodeId,
                                                    const rsp::NodeID& destinationNodeId,
                                                    const std::string& keyId,
                                                    uint64_t lifetimeMs) {
    rsp::proto::RSPMessage message;
    *message.mutable_source() = toProtoNodeId(requesterNodeId);
    *message.mutable_destination() = toProtoNodeId(destinationNodeId);
    auto* request = message.mutable_aes_key_negotiation_request();
    request->mutable_key_id()->set_value(keyId);
    *request->mutable_ephemeral_public_key() = rsp::KeyPair::generateP256().publicKey();
    request->set_requested_lifetime_ms(lifetimeMs);
    request->set_algorithm(rsp::proto::KEY_NEGOTIATION_ALGORITHM_P256_SHA256_AES256);
    return message;
}

bool buffersEqual(const rsp::Buffer& lhs, const rsp::Buffer& rhs) {
    return lhs.size() == rhs.size() &&
           std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

void establishAesKey(CapturingNode& initiator,
                     CapturingNode& responder,
                     uint64_t lifetimeMs = 60000,
                     bool verifyActiveAfterHandshake = true) {
    const auto initiatorLimits = initiator.aesNegotiationLimits();
    const auto responderLimits = responder.aesNegotiationLimits();
    if (initiatorLimits.maxLifetimeMs < initiatorLimits.minLifetimeMs ||
        responderLimits.maxLifetimeMs < responderLimits.minLifetimeMs) {
        throw std::runtime_error(
            "default AES key lifetime range invalid (initiator_min=" +
            std::to_string(initiatorLimits.minLifetimeMs) +
            ", initiator_max=" + std::to_string(initiatorLimits.maxLifetimeMs) +
            ", responder_min=" + std::to_string(responderLimits.minLifetimeMs) +
            ", responder_max=" + std::to_string(responderLimits.maxLifetimeMs) + ")");
    }

    require(initiator.beginAesKeyNegotiation(responder.nodeId(), lifetimeMs),
            "initiator should start AES key negotiation");
    require(waitForCondition([&initiator]() { return initiator.capturedOutputCount() == 1; }),
            "initiator should emit an AES key negotiation request");

    rsp::proto::RSPMessage request;
    require(initiator.tryPopCapturedOutput(request),
            "initiator should provide the AES key negotiation request");
    require(request.has_aes_key_negotiation_request(),
            "initiator output should include AES key negotiation request payload");
    const uint64_t expectedRequestedLifetime = std::clamp(
        lifetimeMs == 0 ? initiatorLimits.defaultLifetimeMs : lifetimeMs,
        initiatorLimits.minLifetimeMs,
        initiatorLimits.maxLifetimeMs);
    require(request.aes_key_negotiation_request().requested_lifetime_ms() == expectedRequestedLifetime,
            "AES key negotiation request should honor initiator lifetime limits");
    require(request.destination().value() == toProtoNodeId(responder.nodeId()).value(),
            "AES negotiation request should target the responder");
    require(responder.enqueueInput(request),
            "responder should accept the AES key negotiation request");
    require(waitForCondition([&responder]() { return responder.capturedOutputCount() == 1; }),
            "responder should emit an AES key negotiation reply");

    rsp::proto::RSPMessage reply;
    require(responder.tryPopCapturedOutput(reply),
            "responder should provide the AES key negotiation reply");
    require(reply.has_aes_key_negotiation_reply(),
            "responder output should include AES key negotiation reply payload");
    const auto acceptedLifetime = reply.aes_key_negotiation_reply().accepted_lifetime_ms();
    require(acceptedLifetime >= responderLimits.minLifetimeMs &&
                acceptedLifetime <= responderLimits.maxLifetimeMs,
            "AES key negotiation reply should honor responder lifetime limits");
    if (verifyActiveAfterHandshake) {
        require(responder.hasAesKey(initiator.nodeId()),
                "responder should install an AES key before sending the negotiation reply");
    }

    require(initiator.enqueueInput(reply),
            "initiator should accept the AES key negotiation reply");
    require(waitForCondition([&initiator]() { return initiator.pendingAesNegotiationCount() == 0; }),
            "initiator should clear pending AES negotiation state after reply processing");
    if (verifyActiveAfterHandshake) {
        require(waitForCondition([&initiator, &responder]() {
            return initiator.hasAesKey(responder.nodeId());
        }), "initiator should install the negotiated AES key");
        require(waitForCondition([&initiator, &responder]() {
            return responder.hasAesKey(initiator.nodeId());
        }), "responder should keep the negotiated AES key");
    }
}

rsp::proto::RSPMessage makeLogSubscribeRequest(const rsp::NodeID& requesterNodeId) {
    rsp::proto::RSPMessage message;
    *message.mutable_source() = toProtoNodeId(requesterNodeId);
    message.mutable_log_subscribe_request()->set_payload_type_url("type.rsp/rsp.proto.LogText");
    message.mutable_log_subscribe_request()->mutable_filter()->mutable_true_value();
    message.mutable_log_subscribe_request()->set_duration_ms(5000);
    return message;
}

void testPingProducesReply() {
    TestNode node;
    node.pauseOutputQueue();

    require(node.enqueueInput(makePingRequest()), "node should accept a ping request on its input queue");
    require(waitForCondition([&node]() { return node.pendingOutputCount() == 1; }),
            "node should enqueue a ping reply on the output queue");

    rsp::proto::RSPMessage reply;
    require(node.tryPopOutput(reply), "node should allow inspection of the generated ping reply");
    require(reply.has_ping_reply(), "node should produce a ping reply for a ping request");
    require(reply.destination().value() == "requester-node", "ping reply should target the original sender");
    require(reply.ping_reply().nonce().value() == "ping-nonce", "ping reply should preserve the request nonce");
    require(reply.ping_reply().sequence() == 7, "ping reply should preserve the request sequence");
    require(reply.ping_reply().time_sent().milliseconds_since_epoch() == 123456,
            "ping reply should preserve the original send time");
    require(reply.ping_reply().has_time_replied(), "ping reply should stamp a reply time");
}

void testUnsupportedMessageProducesError() {
    TestNode node;
    node.pauseOutputQueue();

    require(node.enqueueInput(makeUnsupportedMessage()), "node should accept unsupported messages on its input queue");
    require(waitForCondition([&node]() { return node.pendingOutputCount() == 1; }),
            "node should enqueue an error reply for unsupported messages");

    rsp::proto::RSPMessage reply;
    require(node.tryPopOutput(reply), "node should allow inspection of the generated error reply");
    require(reply.has_error(), "node should produce an error reply for unsupported messages");
    require(reply.error().error_code() == rsp::proto::UNKNOWN_MESSAGE_TYPE,
            "node should classify unsupported messages as unknown message types");
    require(reply.destination().value() == "requester-node", "error reply should target the original sender");
}

void testLogSubscribeProducesReply() {
    TestNode node;
    node.pauseOutputQueue();
    const rsp::NodeID requesterNodeId = rsp::KeyPair::generateP256().nodeID();

    require(node.enqueueInput(makeLogSubscribeRequest(requesterNodeId)),
        "node should accept a log subscribe request on its input queue");
    require(waitForCondition([&node]() { return node.pendingOutputCount() == 1; }),
        "node should enqueue a log subscribe reply on the output queue");

    rsp::proto::RSPMessage reply;
    require(node.tryPopOutput(reply), "node should allow inspection of the generated log subscribe reply");
    require(reply.has_log_subscribe_reply(), "node should produce a log subscribe reply for a log request");
    require(reply.destination().value() == toProtoNodeId(requesterNodeId).value(),
        "log subscribe reply should target the original sender");
    require(reply.log_subscribe_reply().status() == rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED,
        "node should accept valid log subscriptions in the base handler");
    require(reply.log_subscribe_reply().has_subscription_id(),
        "log subscribe reply should include a subscription id");
}

void testIdentityMessagesAreCachedByNodeId() {
    TestNode node;
    rsp::KeyPair identityKey = rsp::KeyPair::generateP256();

    require(node.enqueueInput(makeIdentityMessage(identityKey)),
            "node should accept an identity message on its input queue");
    require(node.identityCache().contains(identityKey.nodeID()),
            "node should cache identity messages using the sender NodeID derived from the public key");

    const auto cachedIdentity = node.identityCache().get(identityKey.nodeID());
    require(cachedIdentity.has_value(), "node should expose cached identities");
    require(cachedIdentity->public_key().public_key() == identityKey.publicKey().public_key(),
            "cached identity should preserve the public key payload");
    require(!cachedIdentity->has_nonce(),
        "node identity cache should not retain the identity nonce");
}

void testIdentityCacheEvictsLeastRecentlyUsedEntries() {
    TestNode node;
    node.identityCache().setMaximumEntries(1);

    rsp::KeyPair firstIdentityKey = rsp::KeyPair::generateP256();
    rsp::KeyPair secondIdentityKey = rsp::KeyPair::generateP256();

    require(node.enqueueInput(makeIdentityMessage(firstIdentityKey)),
            "node should cache the first identity message");
    require(node.enqueueInput(makeIdentityMessage(secondIdentityKey)),
            "node should cache the second identity message");
    require(!node.identityCache().contains(firstIdentityKey.nodeID()),
            "identity cache should evict the least recently used entry when over capacity");
    require(node.identityCache().contains(secondIdentityKey.nodeID()),
            "identity cache should keep the most recent identity after eviction");
}

void testIdentityCacheCanSendChallengeRequests() {
    TestNode node;
    node.pauseOutputQueue();

    const rsp::NodeID targetNodeId = rsp::KeyPair::generateP256().nodeID();
    require(node.identityCache().sendChallengeRequest(targetNodeId),
            "identity cache should send challenge requests through the owning node output queue");
    require(waitForCondition([&node]() { return node.pendingOutputCount() == 1; }),
            "challenge requests should be enqueued on the node output queue");

    rsp::proto::RSPMessage challengeRequest;
    require(node.tryPopOutput(challengeRequest),
            "node should allow inspection of the generated challenge request");
    require(challengeRequest.has_challenge_request(),
            "identity cache helper should emit a challenge request message");
    require(challengeRequest.destination().value() == toProtoNodeId(targetNodeId).value(),
            "challenge request should target the requested node");
    require(challengeRequest.source().value() == toProtoNodeId(node.nodeId()).value(),
            "challenge request should originate from the owning node");
    require(challengeRequest.challenge_request().nonce().value().size() == 16,
            "challenge request helper should generate a 16-byte nonce");
}

void testChallengeRequestProducesSignedIdentityReply() {
    TestNode node;
    node.pauseOutputQueue();

    const rsp::NodeID requesterNodeId = rsp::KeyPair::generateP256().nodeID();
    const std::string nonce = "0123456789abcdef";

    require(node.enqueueInput(makeChallengeRequest(requesterNodeId, nonce)),
            "node should accept challenge requests on its input queue");
    require(waitForCondition([&node]() { return node.pendingOutputCount() == 1; }),
            "node should enqueue an identity reply for challenge requests");

    rsp::proto::RSPMessage reply;
    require(node.tryPopOutput(reply), "node should allow inspection of challenge identity replies");
    require(reply.has_identity(), "challenge requests should produce identity replies");
    require(reply.has_signature(), "challenge identity replies should be signed");
    require(reply.identity().nonce().value() == nonce,
            "challenge identity replies should preserve the challenge nonce");
    require(reply.destination().value() == toProtoNodeId(requesterNodeId).value(),
            "challenge identity replies should target the requesting node");

    const rsp::KeyPair verifyKey = rsp::KeyPair::fromPublicKey(reply.identity().public_key());
    require(rsp::verifyMessageSignature(verifyKey, reply, reply.signature()),
            "challenge identity replies should verify with the advertised public key");
}

void testAesKeyNegotiationRoundTrip() {
    CapturingNode initiator;
    CapturingNode responder;

    establishAesKey(initiator, responder, 60000);
    require(initiator.pendingAesNegotiationCount() == 0,
            "initiator should clear pending negotiation state after a valid reply");
    require(initiator.activeAesKeyCount() == 1,
            "initiator should track one active AES key after negotiation");
    require(responder.activeAesKeyCount() == 1,
            "responder should track one active AES key after negotiation");

    const auto initiatorKey = initiator.aesKeyMaterial(responder.nodeId());
    const auto responderKey = responder.aesKeyMaterial(initiator.nodeId());
    require(initiatorKey.has_value() && responderKey.has_value(),
            "both peers should expose negotiated AES key material");
    require(buffersEqual(*initiatorKey, *responderKey),
            "both peers should derive identical AES key material");
}

void testAesSingleKeyPerPairProducesError() {
    CapturingNode initiator;
    CapturingNode responder;

    establishAesKey(initiator, responder, 60000);

    const rsp::proto::RSPMessage duplicateRequest = makeAesKeyNegotiationRequest(
        initiator.nodeId(), responder.nodeId(), std::string(16, '\x66'), 60000);
    require(responder.enqueueInput(duplicateRequest),
            "responder should accept duplicate AES key negotiation requests");
    require(waitForCondition([&responder]() { return responder.capturedOutputCount() == 1; }),
            "responder should emit an explicit error for duplicate pair negotiation");

    rsp::proto::RSPMessage errorReply;
    require(responder.tryPopCapturedOutput(errorReply),
            "responder should expose duplicate negotiation error output");
    require(errorReply.has_aes_key_negotiation_error(),
            "duplicate pair negotiation should emit aes_key_negotiation_error");
    require(errorReply.aes_key_negotiation_error().error_code() ==
                rsp::proto::KEY_NEGOTIATION_ERROR_PAIR_KEY_EXISTS,
            "duplicate pair negotiation should report pair already has key");
}

void testAesKeyLimitProducesError() {
    CapturingNode firstInitiator;
    CapturingNode secondInitiator;
    CapturingNode responder;

    auto limits = responder.aesNegotiationLimits();
    limits.maxActiveKeys = 1;
    limits.maxPendingNegotiations = 4;
    limits.minLifetimeMs = 1;
    limits.maxLifetimeMs = 10000;
    limits.defaultLifetimeMs = 1000;
    responder.setAesNegotiationLimits(limits);

    establishAesKey(firstInitiator, responder, 60000);

    const rsp::proto::RSPMessage limitedRequest = makeAesKeyNegotiationRequest(
        secondInitiator.nodeId(), responder.nodeId(), std::string(16, '\x44'), 60000);
    require(responder.enqueueInput(limitedRequest),
            "responder should accept new requests after reaching active key capacity");
    require(waitForCondition([&responder]() { return responder.capturedOutputCount() == 1; }),
            "responder should emit a capacity error when active key limit is reached");

    rsp::proto::RSPMessage capacityError;
    require(responder.tryPopCapturedOutput(capacityError),
            "responder should expose active key capacity errors");
    require(capacityError.has_aes_key_negotiation_error(),
            "active key capacity failures should emit aes_key_negotiation_error");
    require(capacityError.aes_key_negotiation_error().error_code() ==
                rsp::proto::KEY_NEGOTIATION_ERROR_KEY_LIMIT_REACHED,
            "active key capacity failures should report key limit reached");
}

void testAesIncorrectReplyProducesError() {
    CapturingNode initiator;
    CapturingNode responder;

    require(initiator.beginAesKeyNegotiation(responder.nodeId(), 60000),
            "initiator should start AES key negotiation");
    require(waitForCondition([&initiator]() { return initiator.capturedOutputCount() == 1; }),
            "initiator should emit a request before receiving a reply");

    rsp::proto::RSPMessage request;
    require(initiator.tryPopCapturedOutput(request), "test should capture the outgoing AES request");
    require(responder.enqueueInput(request), "responder should accept the outgoing AES request");
    require(waitForCondition([&responder]() { return responder.capturedOutputCount() == 1; }),
            "responder should produce an AES negotiation reply");

    rsp::proto::RSPMessage tamperedReply;
    require(responder.tryPopCapturedOutput(tamperedReply), "test should capture the responder reply");
    tamperedReply.mutable_aes_key_negotiation_reply()->mutable_key_id()->set_value(std::string(16, '\x77'));
    require(initiator.enqueueInput(tamperedReply),
            "initiator should accept an incorrect reply and emit an explicit error");
    require(waitForCondition([&initiator]() { return initiator.capturedOutputCount() == 1; }),
            "initiator should emit an explicit incorrect-key error");

    rsp::proto::RSPMessage errorMessage;
    require(initiator.tryPopCapturedOutput(errorMessage),
            "initiator should expose incorrect key reply errors");
    require(errorMessage.has_aes_key_negotiation_error(),
            "incorrect reply should emit aes_key_negotiation_error");
    require(errorMessage.aes_key_negotiation_error().error_code() ==
                rsp::proto::KEY_NEGOTIATION_ERROR_KEY_INCORRECT,
            "incorrect reply should report KEY_NEGOTIATION_ERROR_KEY_INCORRECT");
}

void testAesKeyExpirationSendsDropNotification() {
    CapturingNode initiator;
    CapturingNode responder;

    auto initiatorLimits = initiator.aesNegotiationLimits();
    initiatorLimits.minLifetimeMs = 1;
    initiatorLimits.defaultLifetimeMs = 1;
    initiatorLimits.maxLifetimeMs = 50;
    initiator.setAesNegotiationLimits(initiatorLimits);

    auto responderLimits = responder.aesNegotiationLimits();
    responderLimits.minLifetimeMs = 1;
    responderLimits.defaultLifetimeMs = 1;
    responderLimits.maxLifetimeMs = 50;
    responder.setAesNegotiationLimits(responderLimits);

    establishAesKey(initiator, responder, 1, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    require(!responder.hasAesKey(initiator.nodeId()),
            "expired AES keys should be evicted before key lookups");
    require(waitForCondition([&responder]() { return responder.capturedOutputCount() >= 1; }),
            "expiring an AES key should enqueue an explicit drop notification");

    bool sawDropNotification = false;
    rsp::proto::RSPMessage output;
    while (responder.tryPopCapturedOutput(output)) {
        if (!output.has_aes_key_drop_notification()) {
            continue;
        }
        sawDropNotification = true;
        require(output.destination().value() == toProtoNodeId(initiator.nodeId()).value(),
                "AES key drop notifications should target the peer whose key expired");
        require(output.aes_key_drop_notification().reason() == "expired",
                "expired AES key drop notifications should include reason=expired");
    }
    require(sawDropNotification,
            "expired AES keys should emit an explicit drop notification");
}

void testIdentityCacheCallbacksAreTriggeredForObservedIdentities() {
    TestNode node;
    rsp::KeyPair identityKey = rsp::KeyPair::generateP256();
    rsp::KeyPair secondIdentityKey = rsp::KeyPair::generateP256();
    std::atomic<int> callbackCount{0};

    const auto callbackToken = node.identityCache().addIdentityObservedCallback(
        [&callbackCount](const rsp::NodeID&) {
            ++callbackCount;
        });

    require(callbackToken != 0, "identity cache should return a callback token");
    require(node.enqueueInput(makeIdentityMessage(identityKey)),
            "node should cache identities while callback observers are attached");
    require(callbackCount.load() == 1,
            "identity cache observers should be notified when identities are cached");

    require(node.identityCache().removeIdentityObservedCallback(callbackToken),
            "identity cache should allow callback removal");
    require(node.enqueueInput(makeIdentityMessage(secondIdentityKey)),
            "node should continue caching identities after callback removal");
    require(callbackCount.load() == 1,
            "identity cache should stop notifying removed callbacks");
}

}  // namespace

int main() {
    try {
        testPingProducesReply();
        testUnsupportedMessageProducesError();
        testLogSubscribeProducesReply();
        testIdentityMessagesAreCachedByNodeId();
        testIdentityCacheEvictsLeastRecentlyUsedEntries();
        testIdentityCacheCanSendChallengeRequests();
        testChallengeRequestProducesSignedIdentityReply();
        testAesKeyNegotiationRoundTrip();
        testAesSingleKeyPerPairProducesError();
        testAesKeyLimitProducesError();
        testAesIncorrectReplyProducesError();
        testAesKeyExpirationSendsDropNotification();
        testIdentityCacheCallbacksAreTriggeredForObservedIdentities();
        std::cout << "node_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "node_test failed: " << exception.what() << '\n';
        return 1;
    }
}
