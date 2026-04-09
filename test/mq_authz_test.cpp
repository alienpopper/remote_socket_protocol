#include "common/endorsement/endorsement.hpp"
#include "common/keypair.hpp"
#include "common/message_queue/mq_authz.hpp"

#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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

rsp::proto::RSPMessage makeMessageWithSource(const rsp::NodeID& sourceNodeId) {
    rsp::proto::RSPMessage message;
    *message.mutable_source() = makeProtoSourceNodeId(sourceNodeId);
    message.mutable_ping_request();
    return message;
}

rsp::proto::ERDAbstractSyntaxTree makeTypeEqualsTree(const rsp::GUID& endorsementType) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    std::string value;
    value.reserve(16);
    for (int shift = 56; shift >= 0; shift -= 8) {
        value.push_back(static_cast<char>((endorsementType.high() >> shift) & 0xFFULL));
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        value.push_back(static_cast<char>((endorsementType.low() >> shift) & 0xFFULL));
    }
    tree.mutable_type_equals()->mutable_type()->set_value(value);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeValueEqualsTree(const std::string& value) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_value_equals()->set_value(value);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeAndTree(const rsp::proto::ERDAbstractSyntaxTree& lhs,
                                              const rsp::proto::ERDAbstractSyntaxTree& rhs) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_and_()->mutable_lhs() = lhs;
    *tree.mutable_and_()->mutable_rhs() = rhs;
    return tree;
}

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

rsp::Endorsement makeEndorsement(const rsp::KeyPair& issuer,
                                 const rsp::NodeID& subject,
                                 const rsp::GUID& endorsementType,
                                 const std::string& value,
                                 double lifetimeSeconds) {
    rsp::DateTime validUntil;
    validUntil += lifetimeSeconds;
    return rsp::Endorsement::createSigned(
        issuer,
        subject,
        endorsementType,
        rsp::Buffer(reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size())),
        validUntil);
}

void testAuthZSuccessOnMatchingEndorsement() {
    const rsp::KeyPair issuer = rsp::KeyPair::generateP256();
    const rsp::KeyPair sourceKey = rsp::KeyPair::generateP256();
    const rsp::GUID requiredType("00112233-4455-6677-8899-aabbccddeeff");

    std::promise<rsp::proto::RSPMessage> successPromise;
    auto successFuture = successPromise.get_future();
    std::promise<rsp::proto::RSPMessage> failurePromise;
    auto failureFuture = failurePromise.get_future();

    std::optional<rsp::NodeID> requestedNodeId;
    rsp::message_queue::MessageQueueAuthZ queue(
        [&successPromise](rsp::proto::RSPMessage message) { successPromise.set_value(std::move(message)); },
        [&failurePromise](rsp::proto::RSPMessage message) { failurePromise.set_value(std::move(message)); },
        [&requestedNodeId, &issuer, &sourceKey, &requiredType](const rsp::NodeID& nodeId) {
            requestedNodeId = nodeId;
            return std::vector<rsp::Endorsement>{
                makeEndorsement(issuer, sourceKey.nodeID(), requiredType, "network-access", HOURS(1))};
        },
        makeAndTree(makeTypeEqualsTree(requiredType), makeValueEqualsTree("network-access")));

    queue.setWorkerCount(1);
    queue.start();
    queue.push(makeMessageWithSource(sourceKey.nodeID()));

    const auto authorized = waitFor(successFuture);
    require(authorized.has_source(), "authorized message should preserve source");
    require(requestedNodeId.has_value() && requestedNodeId.value() == sourceKey.nodeID(),
            "authz queue should fetch endorsements for the message source node");
    require(failureFuture.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
            "authz success should not also trigger failure");
}

void testAuthZFailsWithoutSource() {
    std::promise<rsp::proto::RSPMessage> failurePromise;
    auto failureFuture = failurePromise.get_future();

    rsp::message_queue::MessageQueueAuthZ queue(
        [](rsp::proto::RSPMessage) { throw std::runtime_error("unexpected authz success"); },
        [&failurePromise](rsp::proto::RSPMessage message) { failurePromise.set_value(std::move(message)); },
        [](const rsp::NodeID&) { return std::vector<rsp::Endorsement>(); },
        rsp::proto::ERDAbstractSyntaxTree());

    queue.setWorkerCount(1);
    queue.start();
    queue.push(rsp::proto::RSPMessage());

    const auto failed = waitFor(failureFuture);
    require(!failed.has_source(), "failure path should preserve the missing-source message");
}

void testAuthZFailsWhenNoEndorsementMatches() {
    const rsp::KeyPair issuer = rsp::KeyPair::generateP256();
    const rsp::KeyPair sourceKey = rsp::KeyPair::generateP256();
    const rsp::GUID requiredType("00112233-4455-6677-8899-aabbccddeeff");
    const rsp::GUID otherType("ffeeddcc-bbaa-9988-7766-554433221100");

    std::promise<rsp::proto::RSPMessage> failurePromise;
    auto failureFuture = failurePromise.get_future();

    rsp::message_queue::MessageQueueAuthZ queue(
        [](rsp::proto::RSPMessage) { throw std::runtime_error("unexpected authz success"); },
        [&failurePromise](rsp::proto::RSPMessage message) { failurePromise.set_value(std::move(message)); },
        [&issuer, &sourceKey, &otherType](const rsp::NodeID&) {
            return std::vector<rsp::Endorsement>{
                makeEndorsement(issuer, sourceKey.nodeID(), otherType, "network-access", HOURS(1))};
        },
        makeTypeEqualsTree(requiredType));

    queue.setWorkerCount(1);
    queue.start();
    queue.push(makeMessageWithSource(sourceKey.nodeID()));

    const auto failed = waitFor(failureFuture);
    require(failed.has_source(), "failure path should preserve the original message");
}

void testAuthZFailsForExpiredEndorsement() {
    const rsp::KeyPair issuer = rsp::KeyPair::generateP256();
    const rsp::KeyPair sourceKey = rsp::KeyPair::generateP256();
    const rsp::GUID requiredType("00112233-4455-6677-8899-aabbccddeeff");

    std::promise<rsp::proto::RSPMessage> failurePromise;
    auto failureFuture = failurePromise.get_future();

    rsp::message_queue::MessageQueueAuthZ queue(
        [](rsp::proto::RSPMessage) { throw std::runtime_error("unexpected authz success"); },
        [&failurePromise](rsp::proto::RSPMessage message) { failurePromise.set_value(std::move(message)); },
        [&issuer, &sourceKey, &requiredType](const rsp::NodeID&) {
            return std::vector<rsp::Endorsement>{
                makeEndorsement(issuer, sourceKey.nodeID(), requiredType, "network-access", -SECONDS(1))};
        },
        makeTypeEqualsTree(requiredType));

    queue.setWorkerCount(1);
    queue.start();
    queue.push(makeMessageWithSource(sourceKey.nodeID()));

    waitFor(failureFuture);
}

void testAuthZIgnoresWrongSubjectEndorsement() {
    const rsp::KeyPair issuer = rsp::KeyPair::generateP256();
    const rsp::KeyPair sourceKey = rsp::KeyPair::generateP256();
    const rsp::KeyPair otherSubject = rsp::KeyPair::generateP256();
    const rsp::GUID requiredType("00112233-4455-6677-8899-aabbccddeeff");

    std::promise<rsp::proto::RSPMessage> failurePromise;
    auto failureFuture = failurePromise.get_future();

    rsp::message_queue::MessageQueueAuthZ queue(
        [](rsp::proto::RSPMessage) { throw std::runtime_error("unexpected authz success"); },
        [&failurePromise](rsp::proto::RSPMessage message) { failurePromise.set_value(std::move(message)); },
        [&issuer, &otherSubject, &requiredType](const rsp::NodeID&) {
            return std::vector<rsp::Endorsement>{
                makeEndorsement(issuer, otherSubject.nodeID(), requiredType, "network-access", HOURS(1))};
        },
        makeTypeEqualsTree(requiredType));

    queue.setWorkerCount(1);
    queue.start();
    queue.push(makeMessageWithSource(sourceKey.nodeID()));

    waitFor(failureFuture);
}

}  // namespace

int main() {
    try {
        testAuthZSuccessOnMatchingEndorsement();
        testAuthZFailsWithoutSource();
        testAuthZFailsWhenNoEndorsementMatches();
        testAuthZFailsForExpiredEndorsement();
        testAuthZIgnoresWrongSubjectEndorsement();
        std::cout << "mq_authz_test passed" << std::endl;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mq_authz_test failed: " << exception.what() << std::endl;
        return 1;
    }
}