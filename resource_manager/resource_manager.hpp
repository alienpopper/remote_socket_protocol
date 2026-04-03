#pragma once

#include "common/ascii_handshake.hpp"
#include "common/encoding/encoding.hpp"
#include "common/message_queue.hpp"
#include "common/node.hpp"
#include "common/transport/transport.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace rsp::resource_manager {

class ResourceManager : public rsp::RSPNode {
public:
    using NewConnectionCallback = rsp::transport::NewConnectionCallback;

    ResourceManager();
    explicit ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports);

    int run() const override;
    void addClientTransport(const rsp::transport::ListeningTransportHandle& transport);
    size_t clientTransportCount() const;
    void setNewConnectionCallback(NewConnectionCallback callback);
    size_t activeConnectionCount() const;
    size_t activeEncodingCount() const;
    bool sendToConnection(size_t index, const rsp::proto::RSPMessage& message) const;
    bool tryDequeueMessage(rsp::proto::RSPMessage& message) const;
    size_t pendingMessageCount() const;
    bool performAsciiHandshake(const rsp::transport::ConnectionHandle& connection) const;

private:
    void registerTransportCallbacks();
    void registerTransportCallback(const rsp::transport::ListeningTransportHandle& transport);
    void handleNewConnection(const rsp::transport::ConnectionHandle& connection);

    mutable std::mutex connectionsMutex_;
    std::vector<rsp::transport::ConnectionHandle> activeConnections_;
    std::vector<rsp::encoding::EncodingHandle> activeEncodings_;
    mutable std::mutex newConnectionCallbackMutex_;
    NewConnectionCallback newConnectionCallback_;
    rsp::MessageQueueHandle incomingMessages_;
    std::vector<rsp::transport::ListeningTransportHandle> clientTransports_;
};

}  // namespace rsp::resource_manager