#pragma once

#include "common/ascii_handshake.hpp"
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
    bool performAsciiHandshake(const rsp::transport::ConnectionHandle& connection) const;

private:
    void registerTransportCallbacks();
    void registerTransportCallback(const rsp::transport::ListeningTransportHandle& transport);
    void handleNewConnection(const rsp::transport::ConnectionHandle& connection);

    mutable std::mutex connectionsMutex_;
    std::vector<rsp::transport::ConnectionHandle> activeConnections_;
    mutable std::mutex newConnectionCallbackMutex_;
    NewConnectionCallback newConnectionCallback_;
    std::vector<rsp::transport::ListeningTransportHandle> clientTransports_;
};

}  // namespace rsp::resource_manager