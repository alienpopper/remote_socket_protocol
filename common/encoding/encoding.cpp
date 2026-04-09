#include "common/encoding/encoding.hpp"

#include "common/base_types.hpp"
#include "common/ping_trace.hpp"

#include <stdexcept>
#include <thread>
#include <utility>

namespace rsp::encoding {

std::string toProtoNodeIdValue(const rsp::NodeID& nodeId);
std::string classifySendPrefix(const rsp::proto::RSPMessage& message, const rsp::KeyPair& localKeyPair);
std::string classifyReadPrefix(const rsp::proto::RSPMessage& message, const rsp::KeyPair& localKeyPair);
void recordClassifiedEvent(const rsp::proto::RSPMessage& message,
                           const rsp::KeyPair& localKeyPair,
                           const std::string& suffix,
                           bool isSendPath);

namespace {

class EncodingOutgoingQueue : public rsp::RSPMessageQueue {
public:
    explicit EncodingOutgoingQueue(Encoding& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override {
        owner_.dispatchSend(message);
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
    }

private:
    Encoding& owner_;
};

}  // namespace

namespace {
}  // namespace

Encoding::Encoding(rsp::transport::ConnectionHandle connection, rsp::MessageQueueHandle receivedMessages, rsp::KeyPair localKeyPair)
    : connection_(std::move(connection)),
      receivedMessages_(std::move(receivedMessages)),
      outgoingMessages_(std::make_shared<EncodingOutgoingQueue>(*this)),
    localKeyPair_(std::move(localKeyPair)),
    peerNodeId_(std::nullopt),
      running_(false) {
    outgoingMessages_->setWorkerCount(1);
}

Encoding::~Encoding() {
    stop();
}

bool Encoding::start() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (running_ || connection_ == nullptr || receivedMessages_ == nullptr || outgoingMessages_ == nullptr ||
            !localKeyPair_.isValid() || !peerNodeId_.has_value()) {
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    running_ = true;
    outgoingMessages_->start();
    readThread_ = std::thread(&Encoding::readLoop, this);
    return true;
}

void Encoding::stop() {
    rsp::transport::ConnectionHandle connection;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!running_ && !readThread_.joinable()) {
            return;
        }

        running_ = false;
        connection = connection_;
    }

    if (connection != nullptr) {
        connection->close();
    }

    if (outgoingMessages_ != nullptr) {
        outgoingMessages_->stop();
    }

    if (readThread_.joinable() && readThread_.get_id() != std::this_thread::get_id()) {
        readThread_.join();
    }
}

bool Encoding::send(const rsp::proto::RSPMessage& message) {
    return queueSend(message);
}

rsp::MessageQueueHandle Encoding::outgoingMessages() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return outgoingMessages_;
}

rsp::transport::ConnectionHandle Encoding::connection() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return connection_;
}

void Encoding::enqueueReceived(rsp::proto::RSPMessage message) const {
    recordClassifiedEvent(message, localKeyPair_, "received_queue_enqueued", false);
    if (receivedMessages_ != nullptr) {
        receivedMessages_->push(std::move(message));
    }
}

rsp::proto::RSPMessage Encoding::normalizeOutgoingMessage(rsp::proto::RSPMessage message) const {
    if (!message.has_destination() && message.has_challenge_request()) {
        message.clear_signature();
    }

    return message;
}

bool Encoding::queueSend(rsp::proto::RSPMessage message) const {
    const auto queue = outgoingMessages();
    return queue != nullptr && queue->push(normalizeOutgoingMessage(std::move(message)));
}

bool Encoding::dispatchSend(const rsp::proto::RSPMessage& message) {
    recordClassifiedEvent(message, localKeyPair_, "send_worker_start", true);
    std::lock_guard<std::mutex> lock(sendMutex_);
    return writeMessage(message);
}

std::optional<rsp::NodeID> Encoding::peerNodeID() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return peerNodeId_;
}

void Encoding::setPeerNodeID(const rsp::NodeID& nodeId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (peerNodeId_.has_value() && peerNodeId_.value() != nodeId) {
        throw std::logic_error("peer NodeID is already established for this encoding");
    }

    peerNodeId_ = nodeId;
}

const rsp::KeyPair& Encoding::localKeyPair() const {
    return localKeyPair_;
}

void Encoding::readLoop() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!running_) {
                break;
            }
        }

        rsp::proto::RSPMessage message;
        if (!readMessage(message)) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            running_ = false;
            break;
        }

        recordClassifiedEvent(message, localKeyPair_, "read_complete", false);

        enqueueReceived(std::move(message));
    }
}

std::string toProtoNodeIdValue(const rsp::NodeID& nodeId) {
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    return value;
}

std::string classifySendPrefix(const rsp::proto::RSPMessage& message, const rsp::KeyPair& localKeyPair) {
    const std::string localNodeId = toProtoNodeIdValue(localKeyPair.nodeID());

    if (message.has_ping_request()) {
        if (message.source().value() == localNodeId) {
            return "source_request";
        }

        return "rm_forward_request";
    }

    if (message.has_ping_reply() && message.source().value() == localNodeId) {
        return "destination_reply";
    }

    return "";
}

std::string classifyReadPrefix(const rsp::proto::RSPMessage& message, const rsp::KeyPair& localKeyPair) {
    const std::string localNodeId = toProtoNodeIdValue(localKeyPair.nodeID());

    if (message.has_ping_request()) {
        if (message.has_destination() && message.destination().value() == localNodeId) {
            return "destination_request";
        }

        return "rm_request";
    }

    if (message.has_ping_reply() && message.has_destination() && message.destination().value() == localNodeId) {
        return "source_reply";
    }

    return "";
}

void recordClassifiedEvent(const rsp::proto::RSPMessage& message,
                           const rsp::KeyPair& localKeyPair,
                           const std::string& suffix,
                           bool isSendPath) {
    if (!rsp::ping_trace::isEnabled()) {
        return;
    }

    const std::string prefix = isSendPath ? classifySendPrefix(message, localKeyPair)
                                          : classifyReadPrefix(message, localKeyPair);
    if (!prefix.empty()) {
        rsp::ping_trace::recordForMessage(message, prefix + "_" + suffix);
    }
}
}  // namespace rsp::encoding