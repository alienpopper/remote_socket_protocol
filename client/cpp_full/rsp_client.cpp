#include "client/cpp_full/rsp_client.hpp"

#include "common/encoding/protobuf/protobuf_encoding.hpp"
#include "common/transport/transport_tcp.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace rsp::client::full {

namespace {

bool splitTransportSpec(const std::string& transportSpec,
                        std::string& transportName,
                        std::string& transportParameters) {
    const size_t separator = transportSpec.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= transportSpec.size()) {
        return false;
    }

    transportName = transportSpec.substr(0, separator);
    transportParameters = transportSpec.substr(separator + 1);
    return true;
}

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

class IncomingMessageQueue : public rsp::RSPMessageQueue {
public:
    explicit IncomingMessageQueue(RSPClient& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override {
        owner_.dispatchIncomingMessage(std::move(message));
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
        std::cerr << "RSP full client incoming message queue dropped a message because the queue is full" << std::endl;
    }

private:
    RSPClient& owner_;
};

}  // namespace

RSPClient::Ptr RSPClient::create() {
    return Ptr(new RSPClient(KeyPair::generateP256()));
}

RSPClient::Ptr RSPClient::create(KeyPair keyPair) {
    return Ptr(new RSPClient(std::move(keyPair)));
}

RSPClient::RSPClient(KeyPair keyPair)
    : rsp::RSPNode(std::move(keyPair)),
      incomingMessages_(std::make_shared<IncomingMessageQueue>(*this)) {
    incomingMessages_->setWorkerCount(1);
    incomingMessages_->start();
}

RSPClient::~RSPClient() {
    stop();
}

int RSPClient::run() const {
    std::unique_lock<std::mutex> lock(runMutex_);
    runCondition_.wait(lock, [this]() { return stopping_; });
    return 0;
}

void RSPClient::stop() {
    std::map<ClientConnectionID, ClientConnectionState> removedConnections;
    bool shouldNotify = false;

    {
        std::lock_guard<std::mutex> lock(runMutex_);
        if (!stopping_) {
            stopping_ = true;
            shouldNotify = true;
        }
    }

    if (shouldNotify) {
        runCondition_.notify_all();
    }

    if (incomingMessages_ != nullptr) {
        incomingMessages_->stop();
    }

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        removedConnections.swap(connections_);
    }

    for (auto& [_, connectionState] : removedConnections) {
        if (connectionState.encoding != nullptr) {
            connectionState.encoding->stop();
        }

        if (connectionState.transport != nullptr) {
            connectionState.transport->stop();
        }
    }
}

RSPClient::ClientConnectionID RSPClient::connectToResourceManager(const std::string& transportSpec,
                                                                  const std::string& encoding) {
    std::string transportName;
    std::string transportParameters;
    if (!splitTransportSpec(transportSpec, transportName, transportParameters)) {
        throw std::invalid_argument("transport must be in the format <name>:<parameters>");
    }

    const auto selectedTransport = createTransport(transportName);
    if (selectedTransport == nullptr) {
        throw std::invalid_argument("unsupported transport");
    }

    const rsp::transport::ConnectionHandle connection = selectedTransport->connect(transportParameters);
    if (connection == nullptr) {
        throw std::runtime_error("failed to establish transport connection");
    }

    if (!performHandshake(connection, encoding)) {
        selectedTransport->stop();
        throw std::runtime_error("client handshake failed");
    }

    const auto newEncoding = createEncoding(connection, encoding);
    if (newEncoding == nullptr) {
        selectedTransport->stop();
        throw std::invalid_argument("unsupported encoding");
    }

    if (!newEncoding->performInitialIdentityExchange() || !newEncoding->start()) {
        newEncoding->stop();
        selectedTransport->stop();
        throw std::runtime_error("identity handshake failed");
    }

    const ClientConnectionID connectionId;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_.emplace(connectionId, ClientConnectionState{selectedTransport, newEncoding});
    }

    return connectionId;
}

bool RSPClient::send(const rsp::proto::RSPMessage& message) const {
    rsp::MessageQueueHandle selectedQueue;
    size_t selectedQueueSize = 0;
    bool selectionMade = false;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (const auto& [_, connectionState] : connections_) {
            if (connectionState.encoding == nullptr) {
                continue;
            }

            const auto outgoingQueue = connectionState.encoding->outgoingMessages();
            if (outgoingQueue == nullptr) {
                continue;
            }

            const size_t queueSize = outgoingQueue->size();
            if (queueSize == 0) {
                return outgoingQueue->push(message);
            }

            if (!selectionMade || queueSize < selectedQueueSize) {
                selectedQueue = outgoingQueue;
                selectedQueueSize = queueSize;
                selectionMade = true;
            }
        }
    }

    return selectedQueue != nullptr && selectedQueue->push(message);
}

bool RSPClient::sendOnConnection(ClientConnectionID connectionId, const rsp::proto::RSPMessage& message) const {
    const auto selectedConnection = connectionState(connectionId);
    if (!selectedConnection.has_value() || selectedConnection->encoding == nullptr) {
        return false;
    }

    const auto outgoingMessages = selectedConnection->encoding->outgoingMessages();
    return outgoingMessages != nullptr && outgoingMessages->push(message);
}

bool RSPClient::hasConnections() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return !connections_.empty();
}

bool RSPClient::hasConnection(ClientConnectionID connectionId) const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.find(connectionId) != connections_.end();
}

std::size_t RSPClient::connectionCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.size();
}

std::vector<RSPClient::ClientConnectionID> RSPClient::connectionIds() const {
    std::vector<ClientConnectionID> ids;
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    ids.reserve(connections_.size());
    for (const auto& [connectionId, _] : connections_) {
        ids.push_back(connectionId);
    }

    return ids;
}

bool RSPClient::removeConnection(ClientConnectionID connectionId) {
    ClientConnectionState removedConnection;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        const auto iterator = connections_.find(connectionId);
        if (iterator == connections_.end()) {
            return false;
        }

        removedConnection = iterator->second;
        connections_.erase(iterator);
    }

    if (removedConnection.encoding != nullptr) {
        removedConnection.encoding->stop();
    }

    if (removedConnection.transport != nullptr) {
        removedConnection.transport->stop();
    }

    return true;
}

std::optional<rsp::NodeID> RSPClient::peerNodeID(ClientConnectionID connectionId) const {
    const auto selectedConnection = connectionState(connectionId);
    if (!selectedConnection.has_value() || selectedConnection->encoding == nullptr) {
        return std::nullopt;
    }

    return selectedConnection->encoding->peerNodeID();
}

rsp::NodeID RSPClient::nodeId() const {
    return keyPair().nodeID();
}

bool RSPClient::handleNodeSpecificMessage(const rsp::proto::RSPMessage&) {
    return false;
}

void RSPClient::handleOutputMessage(rsp::proto::RSPMessage message) {
    if (!send(message)) {
        std::cerr << "RSP full client failed to send message produced by node handler" << std::endl;
    }
}

rsp::transport::TransportHandle RSPClient::createTransport(const std::string& transportName) const {
    if (transportName == "tcp") {
        return std::make_shared<rsp::transport::TcpTransport>();
    }

    return nullptr;
}

rsp::encoding::EncodingHandle RSPClient::createEncoding(const rsp::transport::ConnectionHandle& connection,
                                                        const std::string& encoding) const {
    if (connection == nullptr) {
        return nullptr;
    }

    if (encoding == rsp::ascii_handshake::kEncoding) {
        return std::make_shared<rsp::encoding::protobuf::ProtobufEncoding>(connection, incomingMessages_, keyPair().duplicate());
    }

    return nullptr;
}

bool RSPClient::performHandshake(const rsp::transport::ConnectionHandle& connection, const std::string& encoding) const {
    if (connection == nullptr || encoding != rsp::ascii_handshake::kEncoding) {
        return false;
    }

    const auto selectedEncoding = rsp::ascii_handshake::performClientHandshake(connection);
    if (!selectedEncoding.has_value() || *selectedEncoding != encoding) {
        return false;
    }

    connection->setNegotiatedEncoding(*selectedEncoding);
    return true;
}

bool RSPClient::isForThisNode(const rsp::proto::RSPMessage& message) const {
    if (!message.has_destination()) {
        return true;
    }

    return message.destination().value() == toProtoNodeId(keyPair().nodeID()).value();
}

void RSPClient::dispatchIncomingMessage(rsp::proto::RSPMessage message) {
    if (!isForThisNode(message)) {
        std::cerr << "RSP full client dropped a message that was not addressed to this node" << std::endl;
        return;
    }

    if (!enqueueInput(std::move(message))) {
        std::cerr << "RSP full client failed to enqueue inbound message on node input queue" << std::endl;
    }
}

std::optional<RSPClient::ClientConnectionState> RSPClient::connectionState(ClientConnectionID connectionId) const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    const auto iterator = connections_.find(connectionId);
    if (iterator == connections_.end()) {
        return std::nullopt;
    }

    return iterator->second;
}

}  // namespace rsp::client::full