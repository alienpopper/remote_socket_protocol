#include "resource_manager/resource_manager.hpp"

#include "common/ascii_handshake.hpp"
#include "common/encoding/protobuf/protobuf_encoding.hpp"
#include "common/ping_trace.hpp"

#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace rsp::resource_manager {

namespace {

rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoNodeId.set_value(value);
    return protoNodeId;
}

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

class IncomingMessageQueue : public rsp::RSPMessageQueue {
public:
    explicit IncomingMessageQueue(ResourceManager& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override {
        if (owner_.isForThisNode(message)) {
            if (!owner_.enqueueInput(std::move(message))) {
                std::cerr << "ResourceManager failed to enqueue local message on input queue" << std::endl;
            }
            return;
        }

        if (!owner_.routeAndSend(message)) {
            std::cerr << "ResourceManager failed to route incoming message" << std::endl;
        }
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
        std::cerr << "ResourceManager incoming message queue dropped message because the queue is full" << std::endl;
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
    : incomingMessages_(std::make_shared<IncomingMessageQueue>(*this)),
      pendingConnections_(std::make_shared<PendingConnectionQueue>(*this)),
      pendingEncodings_(std::make_shared<PendingEncodingQueue>(*this)) {
    incomingMessages_->setWorkerCount(1);
    incomingMessages_->start();
    pendingConnections_->setWorkerCount(1);
    pendingConnections_->start();
    pendingEncodings_->setWorkerCount(1);
    pendingEncodings_->start();
    registerTransportCallbacks();
}

ResourceManager::ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports)
        : incomingMessages_(std::make_shared<IncomingMessageQueue>(*this)),
      pendingConnections_(std::make_shared<PendingConnectionQueue>(*this)),
      pendingEncodings_(std::make_shared<PendingEncodingQueue>(*this)),
      clientTransports_(std::move(clientTransports)) {
        incomingMessages_->setWorkerCount(1);
        incomingMessages_->start();
    pendingConnections_->setWorkerCount(1);
    pendingConnections_->start();
    pendingEncodings_->setWorkerCount(1);
    pendingEncodings_->start();
    registerTransportCallbacks();
}

ResourceManager::~ResourceManager() {
    if (incomingMessages_ != nullptr) {
        incomingMessages_->stop();
    }

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

void ResourceManager::handleOutputMessage(rsp::proto::RSPMessage message) {
    if (!routeAndSend(message)) {
        std::cerr << "ResourceManager failed to route outgoing message" << std::endl;
    }
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
    const bool queued = outgoingMessages != nullptr && outgoingMessages->push(message);
    if (queued && rsp::ping_trace::isEnabled() && message.has_ping_request()) {
        rsp::ping_trace::recordForMessage(message, "rm_forward_send_enqueued");
    }

    return queued;
}

bool ResourceManager::routeAndSend(const rsp::proto::RSPMessage& message) const {
    if (!message.has_destination() || message.destination().value().empty()) {
        return false;
    }

    rsp::encoding::EncodingHandle selectedEncoding;

    {
        std::lock_guard<std::mutex> lock(encodingsMutex_);
        for (const auto& encoding : activeEncodings_) {
            if (encoding == nullptr) {
                continue;
            }

            const auto peerNodeId = encoding->peerNodeID();
            if (!peerNodeId.has_value()) {
                continue;
            }

            if (toProtoNodeId(*peerNodeId).value() == message.destination().value()) {
                selectedEncoding = encoding;
                break;
            }
        }
    }

    if (selectedEncoding == nullptr) {
        return false;
    }

    const auto outgoingMessages = selectedEncoding->outgoingMessages();
    return outgoingMessages != nullptr && outgoingMessages->push(message);
}

bool ResourceManager::isForThisNode(const rsp::proto::RSPMessage& message) const {
    if (!message.has_destination()) {
        return true;
    }

    return message.destination().value() == toProtoNodeId(keyPair().nodeID()).value();
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
        return std::make_shared<rsp::encoding::protobuf::ProtobufEncoding>(connection, incomingMessages_, keyPair().duplicate());
    }

    return nullptr;
}

}  // namespace rsp::resource_manager