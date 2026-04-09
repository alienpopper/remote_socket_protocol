#include "common/message_queue/mq_signing.hpp"
#include "common/keypair.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Encodes a NodeID into a proto NodeId using native-endian memcpy, matching
// the encoding used for source/destination fields in rsp_client.cpp.
rsp::proto::NodeId makeProtoSourceNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoId.set_value(value);
    return protoId;
}

// Builds a minimal RSPMessage with the given source NodeID set.
rsp::proto::RSPMessage makeMessageWithSource(const rsp::NodeID& sourceNodeId) {
    rsp::proto::RSPMessage message;
    *message.mutable_source() = makeProtoSourceNodeId(sourceNodeId);
    message.mutable_ping_request();  // give it a payload so serialization is non-trivial
    return message;
}

// Waits up to 5 seconds for a future to become ready.
template <typename T>
T waitFor(std::future<T>& future) {
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        throw std::runtime_error("timed out waiting for queue result");
    }
    return future.get();
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Sign a message using the getKeyForNodeID constructor and verify the result.
void testSignWithGetKeyFunction() {
    rsp::KeyPair keyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID nodeId = keyPair.nodeID();

    auto sharedKey = std::make_shared<rsp::KeyPair>(std::move(keyPair));

    std::promise<rsp::proto::RSPMessage> successPromise;
    auto successFuture = successPromise.get_future();

    rsp::MessageQueueSign signQueue(
        [&successPromise](rsp::proto::RSPMessage msg) {
            successPromise.set_value(std::move(msg));
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            throw std::runtime_error("unexpected failure: " + reason);
        },
        [sharedKey](const rsp::NodeID& id) -> std::shared_ptr<const rsp::KeyPair> {
            if (id == sharedKey->nodeID()) {
                return sharedKey;
            }
            return nullptr;
        });

    signQueue.setWorkerCount(1);
    signQueue.start();
    signQueue.push(makeMessageWithSource(nodeId));

    const rsp::proto::RSPMessage signed_ = waitFor(successFuture);
    require(signed_.has_signature(), "signed message should have a signature");
}

// Sign a message using the KeyPair convenience constructor.
void testSignWithKeypairConstructor() {
    rsp::KeyPair keyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID nodeId = keyPair.nodeID();
    rsp::KeyPair verifyKey = rsp::KeyPair::fromPublicKey(keyPair.publicKey());

    std::promise<rsp::proto::RSPMessage> successPromise;
    auto successFuture = successPromise.get_future();

    rsp::MessageQueueSign signQueue(
        [&successPromise](rsp::proto::RSPMessage msg) {
            successPromise.set_value(std::move(msg));
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            throw std::runtime_error("unexpected failure: " + reason);
        },
        std::move(keyPair));

    signQueue.setWorkerCount(1);
    signQueue.start();
    signQueue.push(makeMessageWithSource(nodeId));

    const rsp::proto::RSPMessage signed_ = waitFor(successFuture);
    require(signed_.has_signature(), "signed message should have a signature");
}

// Signing should fail when the source NodeID does not match the keypair.
void testSignFailsWhenNodeIdMismatch() {
    rsp::KeyPair keyPair = rsp::KeyPair::generateP256();
    rsp::KeyPair otherPair = rsp::KeyPair::generateP256();
    const rsp::NodeID otherId = otherPair.nodeID();

    std::promise<std::string> failurePromise;
    auto failureFuture = failurePromise.get_future();

    rsp::MessageQueueSign signQueue(
        [](rsp::proto::RSPMessage) {
            throw std::runtime_error("unexpected success");
        },
        [&failurePromise](rsp::proto::RSPMessage, std::string reason) {
            failurePromise.set_value(std::move(reason));
        },
        std::move(keyPair));

    signQueue.setWorkerCount(1);
    signQueue.start();
    // Push message from a different node — keypair constructor should reject it.
    signQueue.push(makeMessageWithSource(otherId));

    const std::string reason = waitFor(failureFuture);
    require(!reason.empty(), "failure reason should not be empty");
}

// Signing should fail when the message has no source field.
void testSignFailsWithNoSource() {
    rsp::KeyPair keyPair = rsp::KeyPair::generateP256();

    std::promise<std::string> failurePromise;
    auto failureFuture = failurePromise.get_future();

    rsp::MessageQueueSign signQueue(
        [](rsp::proto::RSPMessage) {
            throw std::runtime_error("unexpected success");
        },
        [&failurePromise](rsp::proto::RSPMessage, std::string reason) {
            failurePromise.set_value(std::move(reason));
        },
        std::move(keyPair));

    signQueue.setWorkerCount(1);
    signQueue.start();

    rsp::proto::RSPMessage msg;
    // deliberately no source set
    signQueue.push(std::move(msg));

    const std::string reason = waitFor(failureFuture);
    require(!reason.empty(), "failure reason should not be empty");
}

// Check a valid signature using the getKeyForNodeID constructor.
void testCheckSignatureWithGetKeyFunction() {
    rsp::KeyPair keyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID nodeId = keyPair.nodeID();

    // Sign a message first.
    rsp::proto::RSPMessage original = makeMessageWithSource(nodeId);
    {
        rsp::proto::RSPMessage copy = original;
        copy.clear_signature();
        std::string payload;
        copy.SerializeToString(&payload);
        rsp::Buffer buf(reinterpret_cast<const uint8_t*>(payload.data()),
                        static_cast<uint32_t>(payload.size()));
        *original.mutable_signature() = keyPair.signBlock(buf);
    }

    auto publicKey = std::make_shared<rsp::KeyPair>(rsp::KeyPair::fromPublicKey(keyPair.publicKey()));
    const rsp::NodeID publicNodeId = publicKey->nodeID();

    std::promise<rsp::proto::RSPMessage> successPromise;
    auto successFuture = successPromise.get_future();

    rsp::MessageQueueCheckSignature checkQueue(
        [&successPromise](rsp::proto::RSPMessage msg) {
            successPromise.set_value(std::move(msg));
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            throw std::runtime_error("unexpected failure: " + reason);
        },
        [publicKey, publicNodeId](const rsp::NodeID& id) -> std::shared_ptr<const rsp::KeyPair> {
            if (id == publicNodeId) {
                return publicKey;
            }
            return nullptr;
        });

    checkQueue.setWorkerCount(1);
    checkQueue.start();
    checkQueue.push(std::move(original));

    const rsp::proto::RSPMessage verified = waitFor(successFuture);
    require(verified.has_signature(), "verified message should still have its signature");
}

// Check a valid signature using the KeyPair convenience constructor.
void testCheckSignatureWithKeypairConstructor() {
    rsp::KeyPair signingKey = rsp::KeyPair::generateP256();
    const rsp::NodeID nodeId = signingKey.nodeID();
    rsp::KeyPair publicKey = rsp::KeyPair::fromPublicKey(signingKey.publicKey());

    // Sign a message.
    rsp::proto::RSPMessage original = makeMessageWithSource(nodeId);
    {
        rsp::proto::RSPMessage copy = original;
        copy.clear_signature();
        std::string payload;
        copy.SerializeToString(&payload);
        rsp::Buffer buf(reinterpret_cast<const uint8_t*>(payload.data()),
                        static_cast<uint32_t>(payload.size()));
        *original.mutable_signature() = signingKey.signBlock(buf);
    }

    std::promise<rsp::proto::RSPMessage> successPromise;
    auto successFuture = successPromise.get_future();

    rsp::MessageQueueCheckSignature checkQueue(
        [&successPromise](rsp::proto::RSPMessage msg) {
            successPromise.set_value(std::move(msg));
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            throw std::runtime_error("unexpected failure: " + reason);
        },
        std::move(publicKey));

    checkQueue.setWorkerCount(1);
    checkQueue.start();
    checkQueue.push(std::move(original));

    const rsp::proto::RSPMessage verified = waitFor(successFuture);
    require(verified.has_signature(), "verified message should still have its signature");
}

// Verification should fail for a tampered message.
void testCheckSignatureFailsForTamperedMessage() {
    rsp::KeyPair signingKey = rsp::KeyPair::generateP256();
    const rsp::NodeID nodeId = signingKey.nodeID();
    rsp::KeyPair publicKey = rsp::KeyPair::fromPublicKey(signingKey.publicKey());

    // Sign a message.
    rsp::proto::RSPMessage original = makeMessageWithSource(nodeId);
    {
        rsp::proto::RSPMessage copy = original;
        copy.clear_signature();
        std::string payload;
        copy.SerializeToString(&payload);
        rsp::Buffer buf(reinterpret_cast<const uint8_t*>(payload.data()),
                        static_cast<uint32_t>(payload.size()));
        *original.mutable_signature() = signingKey.signBlock(buf);
    }

    // Tamper: add a destination to change the serialized bytes.
    *original.mutable_destination() = makeProtoSourceNodeId(rsp::KeyPair::generateP256().nodeID());

    std::promise<std::string> failurePromise;
    auto failureFuture = failurePromise.get_future();

    rsp::MessageQueueCheckSignature checkQueue(
        [](rsp::proto::RSPMessage) {
            throw std::runtime_error("unexpected success on tampered message");
        },
        [&failurePromise](rsp::proto::RSPMessage, std::string reason) {
            failurePromise.set_value(std::move(reason));
        },
        std::move(publicKey));

    checkQueue.setWorkerCount(1);
    checkQueue.start();
    checkQueue.push(std::move(original));

    const std::string reason = waitFor(failureFuture);
    require(!reason.empty(), "failure reason should not be empty");
}

// Verification should fail when the message has no signature.
void testCheckSignatureFailsWithNoSignature() {
    rsp::KeyPair publicKey = rsp::KeyPair::fromPublicKey(rsp::KeyPair::generateP256().publicKey());

    std::promise<std::string> failurePromise;
    auto failureFuture = failurePromise.get_future();

    rsp::MessageQueueCheckSignature checkQueue(
        [](rsp::proto::RSPMessage) {
            throw std::runtime_error("unexpected success");
        },
        [&failurePromise](rsp::proto::RSPMessage, std::string reason) {
            failurePromise.set_value(std::move(reason));
        },
        std::move(publicKey));

    checkQueue.setWorkerCount(1);
    checkQueue.start();

    rsp::proto::RSPMessage msg;
    // deliberately no signature set
    checkQueue.push(std::move(msg));

    const std::string reason = waitFor(failureFuture);
    require(!reason.empty(), "failure reason should not be empty");
}

// Verification should fail when the signer NodeID is unknown.
void testCheckSignatureFailsWhenKeyNotFound() {
    rsp::KeyPair signingKey = rsp::KeyPair::generateP256();
    const rsp::NodeID nodeId = signingKey.nodeID();
    rsp::KeyPair differentKey = rsp::KeyPair::fromPublicKey(rsp::KeyPair::generateP256().publicKey());

    // Sign with one key but verify with a different (non-matching) key.
    rsp::proto::RSPMessage original = makeMessageWithSource(nodeId);
    {
        rsp::proto::RSPMessage copy = original;
        copy.clear_signature();
        std::string payload;
        copy.SerializeToString(&payload);
        rsp::Buffer buf(reinterpret_cast<const uint8_t*>(payload.data()),
                        static_cast<uint32_t>(payload.size()));
        *original.mutable_signature() = signingKey.signBlock(buf);
    }

    std::promise<std::string> failurePromise;
    auto failureFuture = failurePromise.get_future();

    rsp::MessageQueueCheckSignature checkQueue(
        [](rsp::proto::RSPMessage) {
            throw std::runtime_error("unexpected success");
        },
        [&failurePromise](rsp::proto::RSPMessage, std::string reason) {
            failurePromise.set_value(std::move(reason));
        },
        std::move(differentKey));

    checkQueue.setWorkerCount(1);
    checkQueue.start();
    checkQueue.push(std::move(original));

    const std::string reason = waitFor(failureFuture);
    require(!reason.empty(), "failure reason should not be empty");
}

// Full round-trip: sign via MessageQueueSign then verify via MessageQueueCheckSignature.
void testSignThenCheckRoundTrip() {
    rsp::KeyPair signingKey = rsp::KeyPair::generateP256();
    const rsp::NodeID nodeId = signingKey.nodeID();
    rsp::KeyPair publicKey = rsp::KeyPair::fromPublicKey(signingKey.publicKey());

    std::promise<rsp::proto::RSPMessage> verifiedPromise;
    auto verifiedFuture = verifiedPromise.get_future();

    rsp::MessageQueueCheckSignature checkQueue(
        [&verifiedPromise](rsp::proto::RSPMessage msg) {
            verifiedPromise.set_value(std::move(msg));
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            throw std::runtime_error("check unexpected failure: " + reason);
        },
        std::move(publicKey));
    checkQueue.setWorkerCount(1);
    checkQueue.start();

    rsp::MessageQueueSign signQueue(
        [&checkQueue](rsp::proto::RSPMessage msg) {
            checkQueue.push(std::move(msg));
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            throw std::runtime_error("sign unexpected failure: " + reason);
        },
        std::move(signingKey));
    signQueue.setWorkerCount(1);
    signQueue.start();

    signQueue.push(makeMessageWithSource(nodeId));

    const rsp::proto::RSPMessage result = waitFor(verifiedFuture);
    require(result.has_signature(), "round-trip message should have a signature");
    require(result.has_ping_request(), "round-trip message should retain its payload");
}

int main() {
    try {
        testSignWithGetKeyFunction();
        testSignWithKeypairConstructor();
        testSignFailsWhenNodeIdMismatch();
        testSignFailsWithNoSource();
        testCheckSignatureWithGetKeyFunction();
        testCheckSignatureWithKeypairConstructor();
        testCheckSignatureFailsForTamperedMessage();
        testCheckSignatureFailsWithNoSignature();
        testCheckSignatureFailsWhenKeyNotFound();
        testSignThenCheckRoundTrip();
        std::cout << "mq_signing_test passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "mq_signing_test failed: " << e.what() << std::endl;
        return 1;
    }
}
