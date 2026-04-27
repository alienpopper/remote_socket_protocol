#include "common/node.hpp"
#include "common/message_queue/mq_signing.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
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
        testIdentityCacheCallbacksAreTriggeredForObservedIdentities();
        std::cout << "node_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "node_test failed: " << exception.what() << '\n';
        return 1;
    }
}
