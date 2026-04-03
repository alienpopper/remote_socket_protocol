#include "client/cpp/rsp_client_message.hpp"

#include "common/ascii_handshake.hpp"
#include "common/encoding/protobuf/protobuf_encoding.hpp"
#include "common/transport/transport_tcp.hpp"

#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rsp::client {

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

}  // namespace

RSPClientMessage::Ptr RSPClientMessage::create() {
    return Ptr(new RSPClientMessage(KeyPair::generateP256()));
}

RSPClientMessage::Ptr RSPClientMessage::create(KeyPair keyPair) {
    return Ptr(new RSPClientMessage(std::move(keyPair)));
}

RSPClientMessage::RSPClientMessage(KeyPair keyPair)
    : keyPair_(std::move(keyPair)), incomingMessages_(std::make_shared<rsp::BufferedMessageQueue>()) {
}

int RSPClientMessage::run() const {
    return 0;
}

RSPClientMessage::ClientConnectionID RSPClientMessage::connectToResourceManager(const std::string& transport,
                                                                                const std::string& encoding) {
    std::string transportName;
    std::string transportParameters;
    if (!splitTransportSpec(transport, transportName, transportParameters)) {
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

    if (!performAsciiHandshake(connection, encoding)) {
        selectedTransport->stop();
        throw std::runtime_error("ASCII handshake failed");
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

bool RSPClientMessage::send(const rsp::proto::RSPMessage& message) const {
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

bool RSPClientMessage::sendOnConnection(ClientConnectionID connectionId, const rsp::proto::RSPMessage& message) const {
    const auto selectedConnection = connectionState(connectionId);
    if (!selectedConnection.has_value() || selectedConnection->encoding == nullptr) {
        return false;
    }

    const auto outgoingMessages = selectedConnection->encoding->outgoingMessages();
    return outgoingMessages != nullptr && outgoingMessages->push(message);
}

bool RSPClientMessage::tryDequeueMessage(rsp::proto::RSPMessage& message) const {
    return incomingMessages_ != nullptr && incomingMessages_->tryPop(message);
}

std::size_t RSPClientMessage::pendingMessageCount() const {
    return incomingMessages_ == nullptr ? 0 : incomingMessages_->size();
}

std::optional<rsp::NodeID> RSPClientMessage::peerNodeID(ClientConnectionID connectionId) const {
    const auto selectedConnection = connectionState(connectionId);
    if (!selectedConnection.has_value() || selectedConnection->encoding == nullptr) {
        return std::nullopt;
    }

    return selectedConnection->encoding->peerNodeID();
}

rsp::NodeID RSPClientMessage::nodeId() const {
    return keyPair_.nodeID();
}

bool RSPClientMessage::hasConnections() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return !connections_.empty();
}

bool RSPClientMessage::hasConnection(ClientConnectionID connectionId) const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.find(connectionId) != connections_.end();
}

std::size_t RSPClientMessage::connectionCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.size();
}

std::vector<RSPClientMessage::ClientConnectionID> RSPClientMessage::connectionIds() const {
    std::vector<ClientConnectionID> connectionIds;

    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connectionIds.reserve(connections_.size());
    for (const auto& [connectionId, _] : connections_) {
        connectionIds.push_back(connectionId);
    }

    return connectionIds;
}

bool RSPClientMessage::removeConnection(ClientConnectionID connectionId) {
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

rsp::transport::TransportHandle RSPClientMessage::createTransport(const std::string& transportName) const {
    if (transportName == "tcp") {
        return std::make_shared<rsp::transport::TcpTransport>();
    }

    return nullptr;
}

rsp::encoding::EncodingHandle RSPClientMessage::createEncoding(const rsp::transport::ConnectionHandle& connection,
                                                               const std::string& encoding) const {
    if (connection == nullptr) {
        return nullptr;
    }

    if (encoding == rsp::ascii_handshake::kEncoding) {
        return std::make_shared<rsp::encoding::protobuf::ProtobufEncoding>(connection, incomingMessages_, keyPair_.duplicate());
    }

    return nullptr;
}

bool RSPClientMessage::performAsciiHandshake(const rsp::transport::ConnectionHandle& connection, const std::string& encoding) const {
    if (encoding != rsp::ascii_handshake::kEncoding) {
        return false;
    }

    const auto selectedEncoding = rsp::ascii_handshake::performClientHandshake(connection);
    if (!selectedEncoding.has_value()) {
        return false;
    }

    if (*selectedEncoding != encoding) {
        return false;
    }

    connection->setNegotiatedEncoding(*selectedEncoding);
    return true;
}

std::optional<RSPClientMessage::ClientConnectionState> RSPClientMessage::connectionState(ClientConnectionID connectionId) const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    const auto iterator = connections_.find(connectionId);
    if (iterator == connections_.end()) {
        return std::nullopt;
    }

    return iterator->second;
}

}  // namespace rsp::client