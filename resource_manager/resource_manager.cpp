#include "resource_manager/resource_manager.hpp"

#include "common/ascii_handshake.hpp"

#include <functional>
#include <mutex>
#include <utility>

namespace rsp::resource_manager {

ResourceManager::ResourceManager() {
    registerTransportCallbacks();
}

ResourceManager::ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports)
    : clientTransports_(std::move(clientTransports)) {
    registerTransportCallbacks();
}

int ResourceManager::run() const {
    return 0;
}

void ResourceManager::addClientTransport(const rsp::transport::ListeningTransportHandle& transport) {
    registerTransportCallback(transport);
    clientTransports_.push_back(transport);
}

size_t ResourceManager::clientTransportCount() const {
    return clientTransports_.size();
}

void ResourceManager::setNewConnectionCallback(NewConnectionCallback callback) {
    std::lock_guard<std::mutex> lock(newConnectionCallbackMutex_);
    newConnectionCallback_ = std::move(callback);
}

size_t ResourceManager::activeConnectionCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return activeConnections_.size();
}

bool ResourceManager::performAsciiHandshake(const rsp::transport::ConnectionHandle& connection) const {
    return rsp::ascii_handshake::performServerHandshake(connection);
}

void ResourceManager::registerTransportCallbacks() {
    for (const auto& transport : clientTransports_) {
        registerTransportCallback(transport);
    }
}

void ResourceManager::registerTransportCallback(const rsp::transport::ListeningTransportHandle& transport) {
    if (transport == nullptr) {
        return;
    }

    transport->setNewConnectionCallback([this](const rsp::transport::ConnectionHandle& connection) {
        handleNewConnection(connection);
    });
}

void ResourceManager::handleNewConnection(const rsp::transport::ConnectionHandle& connection) {
    if (connection == nullptr) {
        return;
    }

    if (!performAsciiHandshake(connection)) {
        connection->close();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        activeConnections_.push_back(connection);
    }

    NewConnectionCallback callback;
    {
        std::lock_guard<std::mutex> lock(newConnectionCallbackMutex_);
        callback = newConnectionCallback_;
    }

    if (callback) {
        callback(connection);
    }
}

}  // namespace rsp::resource_manager