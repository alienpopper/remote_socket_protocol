#pragma once

#include "common/keypair.hpp"
#include "common/message_queue.hpp"
#include "common/transport/transport.hpp"
#include "messages.pb.h"

#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace rsp::encoding {

class Encoding {
public:
    Encoding(rsp::transport::ConnectionHandle connection, rsp::MessageQueueHandle receivedMessages, const rsp::KeyPair& localKeyPair);
    virtual ~Encoding();

    Encoding(const Encoding&) = delete;
    Encoding& operator=(const Encoding&) = delete;

    bool performInitialIdentityExchange();
    bool start();
    void stop();
    bool send(const rsp::proto::RSPMessage& message);
    rsp::MessageQueueHandle outgoingMessages() const;
    bool dispatchSend(const rsp::proto::RSPMessage& message);
    std::optional<rsp::NodeID> peerNodeID() const;

protected:
    rsp::transport::ConnectionHandle connection() const;
    void enqueueReceived(rsp::proto::RSPMessage message) const;
    rsp::proto::RSPMessage normalizeOutgoingMessage(rsp::proto::RSPMessage message) const;
    void setPeerNodeID(const rsp::NodeID& nodeId);

private:
    virtual bool readMessage(rsp::proto::RSPMessage& message) = 0;
    virtual bool writeMessage(const rsp::proto::RSPMessage& message) = 0;
    void readLoop();
    bool queueSend(rsp::proto::RSPMessage message) const;

    mutable std::mutex stateMutex_;
    mutable std::mutex sendMutex_;
    rsp::transport::ConnectionHandle connection_;
    rsp::MessageQueueHandle receivedMessages_;
    rsp::MessageQueueHandle outgoingMessages_;
    const rsp::KeyPair* localKeyPair_;
    std::optional<rsp::NodeID> peerNodeId_;
    bool running_;
    std::thread readThread_;
};

using EncodingHandle = std::shared_ptr<Encoding>;

}  // namespace rsp::encoding