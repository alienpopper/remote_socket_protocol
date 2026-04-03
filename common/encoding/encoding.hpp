#pragma once

#include "common/message_queue.hpp"
#include "common/transport/transport.hpp"
#include "messages.pb.h"

#include <memory>
#include <mutex>
#include <thread>

namespace rsp::encoding {

class Encoding {
public:
    Encoding(rsp::transport::ConnectionHandle connection, rsp::MessageQueueHandle incomingMessages);
    virtual ~Encoding();

    Encoding(const Encoding&) = delete;
    Encoding& operator=(const Encoding&) = delete;

    bool start();
    void stop();
    bool send(const rsp::proto::RSPMessage& message);

protected:
    rsp::transport::ConnectionHandle connection() const;
    void enqueue(rsp::proto::RSPMessage message) const;

private:
    virtual bool readMessage(rsp::proto::RSPMessage& message) = 0;
    virtual bool writeMessage(const rsp::proto::RSPMessage& message) = 0;
    void readLoop();

    mutable std::mutex stateMutex_;
    mutable std::mutex sendMutex_;
    rsp::transport::ConnectionHandle connection_;
    rsp::MessageQueueHandle incomingMessages_;
    bool running_;
    std::thread readThread_;
};

using EncodingHandle = std::shared_ptr<Encoding>;

}  // namespace rsp::encoding