#include "common/node.hpp"

#include "common/base_types.hpp"
#include "os/os_random.hpp"

#include <cstring>
#include <iostream>
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

}  // namespace

void NodeInputQueue::handleMessage(Message message, rsp::MessageQueueSharedState&) {
    if (owner_.handleNodeSpecificMessage(message)) {
        return;
    }

    rsp::proto::RSPMessage reply;
    *reply.mutable_source() = toProtoNodeId(owner_.keyPair().nodeID());
    if (!message.source().value().empty()) {
        *reply.mutable_destination() = message.source();
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
    outputQueue_(std::make_shared<NodeOutputQueue>(*this)) {
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
    return inputQueue_ != nullptr && inputQueue_->push(std::move(message));
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

}  // namespace rsp
