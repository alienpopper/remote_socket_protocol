#include "common/encoding/encoding.hpp"

#include <thread>
#include <utility>

namespace rsp::encoding {
Encoding::Encoding(rsp::transport::ConnectionHandle connection, rsp::MessageQueueHandle incomingMessages)
    : connection_(std::move(connection)), incomingMessages_(std::move(incomingMessages)), running_(false) {
}

Encoding::~Encoding() {
    stop();
}

bool Encoding::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (running_ || connection_ == nullptr || incomingMessages_ == nullptr) {
        return false;
    }

    running_ = true;
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

    if (readThread_.joinable() && readThread_.get_id() != std::this_thread::get_id()) {
        readThread_.join();
    }
}

bool Encoding::send(const rsp::proto::RSPMessage& message) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    return writeMessage(message);
}

rsp::transport::ConnectionHandle Encoding::connection() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return connection_;
}

void Encoding::enqueue(rsp::proto::RSPMessage message) const {
    if (incomingMessages_ != nullptr) {
        incomingMessages_->push(std::move(message));
    }
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

        enqueue(std::move(message));
    }
}

}  // namespace rsp::encoding