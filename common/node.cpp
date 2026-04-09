#include "common/node.hpp"

#include "common/base_types.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "os/os_random.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace rsp {

class NodeInputQueue : public rsp::RSPMessageQueue {
public:
    explicit NodeInputQueue(RSPNode& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override;

    void handleQueueFull(size_t, size_t, const Message&) override {
        std::cerr << "RSPNode input queue dropped message because the queue is full" << std::endl;
    }

private:
    RSPNode& owner_;
};

class NodeOutputQueue : public rsp::RSPMessageQueue {
public:
    explicit NodeOutputQueue(RSPNode& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override {
        owner_.handleOutputMessage(std::move(message));
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
        std::cerr << "RSPNode output queue dropped message because the queue is full" << std::endl;
    }

private:
    RSPNode& owner_;
};

namespace {

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

rsp::proto::DateTime toProtoDateTime(const rsp::DateTime& dateTime) {
    rsp::proto::DateTime protoDateTime;
    protoDateTime.set_milliseconds_since_epoch(dateTime.millisecondsSinceEpoch());
    return protoDateTime;
}

std::optional<rsp::NodeID> fromProtoNodeId(const rsp::proto::NodeId& nodeId) {
    if (nodeId.value().size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, nodeId.value().data(), sizeof(high));
    std::memcpy(&low, nodeId.value().data() + sizeof(high), sizeof(low));
    return rsp::NodeID(high, low);
}

std::optional<rsp::NodeID> nodeIdFromIdentity(const rsp::proto::Identity& identity) {
    try {
        const rsp::KeyPair identityKey = rsp::KeyPair::fromPublicKey(identity.public_key());
        return identityKey.nodeID();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string randomChallengeNonce() {
    std::string nonce(16, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(nonce.data()), static_cast<uint32_t>(nonce.size()));
    return nonce;
}

rsp::proto::Identity sanitizedIdentityForCache(const rsp::proto::Identity& identity) {
    rsp::proto::Identity cachedIdentity;
    *cachedIdentity.mutable_public_key() = identity.public_key();
    return cachedIdentity;
}

}  // namespace

IdentityCache::IdentityCache(NodeID localNodeId, SendMessageCallback sendMessageCallback, size_t maximumEntries)
    : maximumEntries_(maximumEntries),
      localNodeId_(localNodeId),
      sendMessageCallback_(std::move(sendMessageCallback)) {
}

bool IdentityCache::observeMessage(const rsp::proto::RSPMessage& message) {
    if (!message.has_identity()) {
        return false;
    }

    const auto sourceNodeId = rsp::senderNodeIdFromMessage(message);
    return cacheIdentity(message.identity(), sourceNodeId);
}

bool IdentityCache::sendChallengeRequest(const rsp::NodeID& nodeId) const {
    SendMessageCallback sendMessageCallback;
    std::optional<rsp::NodeID> localNodeId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sendMessageCallback = sendMessageCallback_;
        localNodeId = localNodeId_;
    }

    if (!sendMessageCallback || !localNodeId.has_value()) {
        return false;
    }

    rsp::proto::RSPMessage challengeRequest;
    *challengeRequest.mutable_source() = toProtoNodeId(*localNodeId);
    *challengeRequest.mutable_destination() = toProtoNodeId(nodeId);
    challengeRequest.mutable_challenge_request()->mutable_nonce()->set_value(randomChallengeNonce());
    return sendMessageCallback(std::move(challengeRequest));
}

std::optional<rsp::proto::Identity> IdentityCache::get(const rsp::NodeID& nodeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = entries_.find(nodeId);
    if (iterator == entries_.end()) {
        return std::nullopt;
    }

    return iterator->second.identity;
}

bool IdentityCache::contains(const rsp::NodeID& nodeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(nodeId) != entries_.end();
}

size_t IdentityCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t IdentityCache::maximumEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maximumEntries_;
}

void IdentityCache::setMaximumEntries(size_t maximumEntries) {
    std::lock_guard<std::mutex> lock(mutex_);
    maximumEntries_ = maximumEntries;
    trimToLimitLocked();
}

bool IdentityCache::cacheIdentity(const rsp::proto::Identity& identity,
                                  const std::optional<rsp::NodeID>& sourceNodeId) {
    const auto derivedNodeId = nodeIdFromIdentity(identity);
    if (!derivedNodeId.has_value()) {
        return false;
    }

    const rsp::proto::Identity cachedIdentity = sanitizedIdentityForCache(identity);

    if (sourceNodeId.has_value() && *sourceNodeId != *derivedNodeId) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = entries_.find(*derivedNodeId);
    if (existing != entries_.end()) {
        usageOrder_.erase(existing->second.usage);
        usageOrder_.push_front(*derivedNodeId);
        existing->second.identity = cachedIdentity;
        existing->second.usage = usageOrder_.begin();
        return true;
    }

    usageOrder_.push_front(*derivedNodeId);
    entries_[*derivedNodeId] = CacheEntry{cachedIdentity, usageOrder_.begin()};
    trimToLimitLocked();
    return true;
}

void IdentityCache::trimToLimitLocked() {
    while (entries_.size() > maximumEntries_) {
        const rsp::NodeID leastRecent = usageOrder_.back();
        usageOrder_.pop_back();
        entries_.erase(leastRecent);
    }
}

void NodeInputQueue::handleMessage(Message message, rsp::MessageQueueSharedState&) {
    if (owner_.handleNodeSpecificMessage(message)) {
        return;
    }

    rsp::proto::RSPMessage reply;
    const auto senderNodeId = rsp::senderNodeIdFromMessage(message);
    if (senderNodeId.has_value()) {
        *reply.mutable_destination() = toProtoNodeId(*senderNodeId);
    }
    if (message.has_nonce()) {
        *reply.mutable_nonce() = message.nonce();
    }

    if (message.has_ping_request()) {
        auto* pingReply = reply.mutable_ping_reply();
        pingReply->mutable_nonce()->CopyFrom(message.ping_request().nonce());
        pingReply->set_sequence(message.ping_request().sequence());
        pingReply->mutable_time_sent()->CopyFrom(message.ping_request().time_sent());
        *pingReply->mutable_time_replied() = toProtoDateTime(rsp::DateTime());
    } else {
        auto* errorReply = reply.mutable_error();
        errorReply->set_error_code(rsp::proto::UNKNOWN_MESSAGE_TYPE);
        errorReply->set_message("unsupported message for base node handler");
    }

    const auto queue = owner_.outputQueue();
    if (queue == nullptr || !queue->push(std::move(reply))) {
        std::cerr << "RSPNode failed to enqueue reply on the output queue" << std::endl;
    }
}

RSPNode::RSPNode() : RSPNode(KeyPair::generateP256()) {
}

RSPNode::RSPNode(KeyPair keyPair)
    : keyPair_(std::move(keyPair)),
      inputQueue_(std::make_shared<NodeInputQueue>(*this)),
            outputQueue_(std::make_shared<NodeOutputQueue>(*this)),
            identityCache_(keyPair_.nodeID(), [this](rsp::proto::RSPMessage message) {
                    return outputQueue_ != nullptr && outputQueue_->push(std::move(message));
            }) {
    rsp::os::randomFill(instanceSeed_.data(), static_cast<uint32_t>(instanceSeed_.size()));
    inputQueue_->setWorkerCount(1);
    inputQueue_->start();
    outputQueue_->setWorkerCount(1);
    outputQueue_->start();
}

RSPNode::~RSPNode() {
    if (inputQueue_ != nullptr) {
        inputQueue_->stop();
    }

    if (outputQueue_ != nullptr) {
        outputQueue_->stop();
    }
}

bool RSPNode::enqueueInput(rsp::proto::RSPMessage message) const {
    observeMessage(message);
    return inputQueue_ != nullptr && inputQueue_->push(std::move(message));
}

void RSPNode::observeMessage(const rsp::proto::RSPMessage& message) const {
    identityCache_.observeMessage(message);
}

size_t RSPNode::pendingInputCount() const {
    return inputQueue_ == nullptr ? 0 : inputQueue_->size();
}

size_t RSPNode::pendingOutputCount() const {
    return outputQueue_ == nullptr ? 0 : outputQueue_->size();
}

const std::array<uint8_t, 16>& RSPNode::instanceSeed() const {
    return instanceSeed_;
}

const KeyPair& RSPNode::keyPair() const {
    return keyPair_;
}

rsp::MessageQueueHandle RSPNode::inputQueue() const {
    return inputQueue_;
}

rsp::MessageQueueHandle RSPNode::outputQueue() const {
    return outputQueue_;
}

IdentityCache& RSPNode::identityCache() {
    return identityCache_;
}

const IdentityCache& RSPNode::identityCache() const {
    return identityCache_;
}

}  // namespace rsp
