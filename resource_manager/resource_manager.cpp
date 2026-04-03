#include "resource_manager/resource_manager.hpp"

#include "common/ascii_handshake.hpp"
#include "common/encoding/protobuf/protobuf_encoding.hpp"

#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace rsp::resource_manager {

namespace {

class PendingConnectionQueue : public rsp::MessageQueue<rsp::transport::ConnectionHandle> {
public:
    explicit PendingConnectionQueue(ResourceManager& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message connection, rsp::MessageQueueSharedState&) override {
        owner_.processAcceptedConnection(std::move(connection));
    }

    void handleQueueFull(size_t, size_t, const Message& connection) override {
        if (connection != nullptr) {
            connection->close();
        }
    }

private:
    ResourceManager& owner_;
};

class PendingEncodingQueue : public rsp::MessageQueue<rsp::encoding::EncodingHandle> {
public:
    explicit PendingEncodingQueue(ResourceManager& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message encoding, rsp::MessageQueueSharedState&) override {
        owner_.processPendingEncoding(std::move(encoding));
    }

    void handleQueueFull(size_t, size_t, const Message& encoding) override {
        if (encoding != nullptr) {
            encoding->stop();
        }
    }

private:
    ResourceManager& owner_;
};

}  // namespace

ResourceManager::ResourceManager()
    : incomingMessages_(std::make_shared<rsp::BufferedMessageQueue>()),
      pendingConnections_(std::make_shared<PendingConnectionQueue>(*this)),
      pendingEncodings_(std::make_shared<PendingEncodingQueue>(*this)) {
    pendingConnections_->setWorkerCount(1);
    pendingConnections_->start();
    pendingEncodings_->setWorkerCount(1);
    pendingEncodings_->start();
    registerTransportCallbacks();
}

ResourceManager::ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports)
    : incomingMessages_(std::make_shared<rsp::BufferedMessageQueue>()),
      pendingConnections_(std::make_shared<PendingConnectionQueue>(*this)),
      pendingEncodings_(std::make_shared<PendingEncodingQueue>(*this)),
      clientTransports_(std::move(clientTransports)) {
    pendingConnections_->setWorkerCount(1);
    pendingConnections_->start();
    pendingEncodings_->setWorkerCount(1);
    pendingEncodings_->start();
    registerTransportCallbacks();
}

ResourceManager::~ResourceManager() {
    if (pendingConnections_ != nullptr) {
        pendingConnections_->stop();
    }

    if (pendingEncodings_ != nullptr) {
        pendingEncodings_->stop();
    }

    std::vector<rsp::encoding::EncodingHandle> encodings;
    {
        std::lock_guard<std::mutex> lock(encodingsMutex_);
        encodings.swap(activeEncodings_);
    }

    for (const auto& encoding : encodings) {
        if (encoding != nullptr) {
            encoding->stop();
        }
    }

    for (const auto& transport : clientTransports_) {
        if (transport != nullptr) {
            transport->stop();
        }
    }
}

int ResourceManager::run() const {
    return 0;
}

bool ResourceManager::handleNodeSpecificMessage(const rsp::proto::RSPMessage&) {
    return false;
}

void ResourceManager::handleOutputMessage(rsp::proto::RSPMessage) {
    std::cerr << "ResourceManager output queue dropped message" << std::endl;
}

void ResourceManager::addClientTransport(const rsp::transport::ListeningTransportHandle& transport) {
    registerTransportCallback(transport);
    clientTransports_.push_back(transport);
}

size_t ResourceManager::clientTransportCount() const {
    return clientTransports_.size();
}

void ResourceManager::setNewEncodingCallback(NewEncodingCallback callback) {
    std::lock_guard<std::mutex> lock(newEncodingCallbackMutex_);
    newEncodingCallback_ = std::move(callback);
}

size_t ResourceManager::activeEncodingCount() const {
    std::lock_guard<std::mutex> lock(encodingsMutex_);
    return activeEncodings_.size();
}

bool ResourceManager::sendToConnection(size_t index, const rsp::proto::RSPMessage& message) const {
    rsp::encoding::EncodingHandle selectedEncoding;

    {
        std::lock_guard<std::mutex> lock(encodingsMutex_);
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
        enqueueAcceptedConnection(connection);
    });
}

void ResourceManager::enqueueAcceptedConnection(const rsp::transport::ConnectionHandle& connection) {
    if (connection == nullptr) {
        return;
    }

    if (pendingConnections_ == nullptr || !pendingConnections_->push(connection)) {
        connection->close();
    }
}

void ResourceManager::processAcceptedConnection(rsp::transport::ConnectionHandle connection) {
    if (connection == nullptr) {
        return;
    }

    const auto selectedEncoding = rsp::ascii_handshake::performServerHandshake(connection);
    if (!selectedEncoding.has_value()) {
        connection->close();
        return;
    }

    connection->setNegotiatedEncoding(*selectedEncoding);

    const auto encoding = createEncodingForConnection(connection);
    if (encoding == nullptr || pendingEncodings_ == nullptr || !pendingEncodings_->push(encoding)) {
        connection->close();
    }
}

void ResourceManager::processPendingEncoding(rsp::encoding::EncodingHandle encoding) {
    if (encoding == nullptr) {
        return;
    }

    if (!encoding->performInitialIdentityExchange() || !encoding->start()) {
        encoding->stop();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(encodingsMutex_);
        activeEncodings_.push_back(encoding);
    }

    NewEncodingCallback callback;
    {
        std::lock_guard<std::mutex> lock(newEncodingCallbackMutex_);
        callback = newEncodingCallback_;
    }

    if (callback) {
        callback(encoding);
    }
}

rsp::encoding::EncodingHandle ResourceManager::createEncodingForConnection(const rsp::transport::ConnectionHandle& connection) const {
    if (connection == nullptr) {
        return nullptr;
    }

    const auto selectedEncoding = connection->negotiatedEncoding();
    if (!selectedEncoding.has_value()) {
        return nullptr;
    }

    if (*selectedEncoding == rsp::ascii_handshake::kEncoding) {
        return std::make_shared<rsp::encoding::protobuf::ProtobufEncoding>(connection, incomingMessages_, keyPair());
    }

    return nullptr;
}

}  // namespace rsp::resource_manager