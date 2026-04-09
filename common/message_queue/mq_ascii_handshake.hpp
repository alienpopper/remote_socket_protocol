#pragma once

#include "common/encoding/encoding.hpp"
#include "common/keypair.hpp"
#include "common/message_queue/mq.hpp"
#include "common/transport/transport.hpp"

#include <functional>
#include <string>

namespace rsp::message_queue {

constexpr const char* kAsciiHandshakeEncoding = "protobuf";
constexpr const char* kJsonHandshakeEncoding = "json";
constexpr const char* kAsciiHandshakeAsymmetricAlgorithm = "P256";

class MessageQueueAsciiHandshakeServer : public rsp::MessageQueue<rsp::transport::ConnectionHandle> {
public:
    using SuccessCallback = std::function<void(const rsp::encoding::EncodingHandle& newEncoding)>;
    using FailureCallback = std::function<void(const rsp::transport::ConnectionHandle& transport)>;

    MessageQueueAsciiHandshakeServer(rsp::MessageQueueHandle receivedMessages,
                                     rsp::KeyPair localKeyPair,
                                     SuccessCallback success,
                                     FailureCallback failure);

protected:
    void handleMessage(Message transport, rsp::MessageQueueSharedState& sharedState) override;
    void handleQueueFull(size_t currentSize, size_t limit, const Message& transport) override;

private:
    rsp::MessageQueueHandle receivedMessages_;
    rsp::KeyPair localKeyPair_;
    SuccessCallback success_;
    FailureCallback failure_;
};

class MessageQueueAsciiHandshakeClient : public rsp::MessageQueue<rsp::transport::TransportHandle> {
public:
    using SuccessCallback = std::function<void(const rsp::encoding::EncodingHandle& newEncoding)>;
    using FailureCallback = std::function<void(const rsp::transport::TransportHandle& transport)>;

    MessageQueueAsciiHandshakeClient(rsp::MessageQueueHandle receivedMessages,
                                     rsp::KeyPair localKeyPair,
                                     std::string requestedEncoding,
                                     SuccessCallback success,
                                     FailureCallback failure);

protected:
    void handleMessage(Message transport, rsp::MessageQueueSharedState& sharedState) override;
    void handleQueueFull(size_t currentSize, size_t limit, const Message& transport) override;

private:
    rsp::MessageQueueHandle receivedMessages_;
    rsp::KeyPair localKeyPair_;
    std::string requestedEncoding_;
    SuccessCallback success_;
    FailureCallback failure_;
};

}  // namespace rsp::message_queue