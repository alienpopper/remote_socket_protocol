#include "resource_manager/resource_manager.hpp"

#include "common/ascii_handshake.hpp"
#include "common/encoding/protobuf/protobuf_encoding.hpp"

#include <functional>
#include <mutex>
#include <utility>

namespace rsp::resource_manager {

ResourceManager::ResourceManager() : incomingMessages_(std::make_shared<rsp::BufferedMessageQueue>()) {
    registerTransportCallbacks();
}

ResourceManager::ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports)
    : incomingMessages_(std::make_shared<rsp::BufferedMessageQueue>()), clientTransports_(std::move(clientTransports)) {
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

size_t ResourceManager::activeEncodingCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return activeEncodings_.size();
}

bool ResourceManager::sendToConnection(size_t index, const rsp::proto::RSPMessage& message) const {
    rsp::encoding::EncodingHandle selectedEncoding;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        if (index >= activeEncodings_.size()) {
            return false;
        }

        selectedEncoding = activeEncodings_[index];
    }

    if (selectedEncoding == nullptr) {
        return false;
    }

    const auto outgoingMessages = selectedEncoding->outgoingMessages();
    return outgoingMessages != nullptr && outgoingMessages->push(message);
}

bool ResourceManager::tryDequeueMessage(rsp::proto::RSPMessage& message) const {
    return incomingMessages_ != nullptr && incomingMessages_->tryPop(message);
}

size_t ResourceManager::pendingMessageCount() const {
    return incomingMessages_ == nullptr ? 0 : incomingMessages_->size();
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

    const auto encoding = std::make_shared<rsp::encoding::protobuf::ProtobufEncoding>(connection, incomingMessages_, keyPair());
    if (!encoding->start()) {
        connection->close();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        activeConnections_.push_back(connection);
        activeEncodings_.push_back(encoding);
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