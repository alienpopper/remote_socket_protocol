#include "common/encoding/encoding.hpp"

#include <thread>
#include <utility>

namespace rsp::encoding {

namespace {

class EncodingOutgoingQueue : public rsp::MessageQueue {
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

Encoding::Encoding(rsp::transport::ConnectionHandle connection, rsp::MessageQueueHandle receivedMessages)
    : connection_(std::move(connection)),
      receivedMessages_(std::move(receivedMessages)),
      outgoingMessages_(std::make_shared<EncodingOutgoingQueue>(*this)),
      running_(false) {
    outgoingMessages_->setWorkerCount(1);
}

Encoding::~Encoding() {
    stop();
}

bool Encoding::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (running_ || connection_ == nullptr || receivedMessages_ == nullptr || outgoingMessages_ == nullptr) {
        return false;
    }

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
    if (receivedMessages_ != nullptr) {
        receivedMessages_->push(std::move(message));
    }
}

bool Encoding::queueSend(rsp::proto::RSPMessage message) const {
    const auto queue = outgoingMessages();
    return queue != nullptr && queue->push(std::move(message));
}

bool Encoding::dispatchSend(const rsp::proto::RSPMessage& message) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    return writeMessage(message);
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

        enqueueReceived(std::move(message));
    }
}

}  // namespace rsp::encoding