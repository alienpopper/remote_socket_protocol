#include "client/cpp/rsp_client_message.hpp"

#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/message_queue/mq_authn.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "common/transport/transport_memory.hpp"
#include "common/transport/transport_tcp.hpp"
#include "os/os_random.hpp"

#include <chrono>
#include <cstring>
#include <condition_variable>
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

std::string randomMessageNonce() {
    std::string nonce(16, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(nonce.data()), static_cast<uint32_t>(nonce.size()));
    return nonce;
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

    struct ConnectResult {
        std::mutex mutex;
        std::condition_variable condition;
        bool complete = false;
        std::string error;
        rsp::encoding::EncodingHandle encoding;
    } result;

    auto finish = [&](rsp::encoding::EncodingHandle establishedEncoding, std::string error) {
        std::lock_guard<std::mutex> lock(result.mutex);
        result.encoding = std::move(establishedEncoding);
        result.error = std::move(error);
        result.complete = true;
        result.condition.notify_all();
    };

    const auto authnQueue = std::make_shared<rsp::message_queue::MessageQueueAuthN>(
        keyPair_.duplicate(),
        [&](const rsp::encoding::EncodingHandle& establishedEncoding) { finish(establishedEncoding, std::string()); },
        [&](const rsp::encoding::EncodingHandle& establishedEncoding) {
            if (establishedEncoding != nullptr) {
                establishedEncoding->stop();
            }
            finish(nullptr, "identity handshake failed");
        },
        [](const rsp::NodeID&, const rsp::proto::Identity&) {});
    authnQueue->setWorkerCount(1);
    authnQueue->start();

    const auto handshakeQueue = std::make_shared<rsp::message_queue::MessageQueueAsciiHandshakeClient>(
        incomingMessages_,
        keyPair_.duplicate(),
        encoding,
        [authnQueue, &finish](const rsp::encoding::EncodingHandle& establishedEncoding) {
            if (authnQueue == nullptr || !authnQueue->push(establishedEncoding)) {
                if (establishedEncoding != nullptr) {
                    establishedEncoding->stop();
                }
                finish(nullptr, "failed to enqueue encoding for identity handshake");
            }
        },
        [selectedTransport, &finish](const rsp::transport::TransportHandle&) {
            if (selectedTransport != nullptr) {
                selectedTransport->stop();
            }
            finish(nullptr, "ASCII handshake failed");
        });
    handshakeQueue->setWorkerCount(1);
    handshakeQueue->start();

    if (!handshakeQueue->push(selectedTransport)) {
        handshakeQueue->stop();
        authnQueue->stop();
        selectedTransport->stop();
        throw std::runtime_error("failed to enqueue transport for client handshake");
    }

    {
        std::unique_lock<std::mutex> lock(result.mutex);
        result.condition.wait(lock, [&result]() { return result.complete; });
    }

    handshakeQueue->stop();
    authnQueue->stop();

    if (result.encoding == nullptr) {
        selectedTransport->stop();
        throw std::runtime_error(result.error.empty() ? "connection setup failed" : result.error);
    }

    if (!result.encoding->start()) {
        result.encoding->stop();
        selectedTransport->stop();
        throw std::runtime_error("failed to start encoding");
    }

    const auto signingQueue = std::make_shared<rsp::MessageQueueSign>(
        [encodingHandle = result.encoding](rsp::proto::RSPMessage message) {
            const auto outgoingMessages = encodingHandle == nullptr ? nullptr : encodingHandle->outgoingMessages();
            if (outgoingMessages == nullptr || !outgoingMessages->push(std::move(message))) {
                std::cerr << "RSP client failed to enqueue signed message for transport" << std::endl;
            }
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            std::cerr << "RSP client failed to sign outbound message: " << reason << std::endl;
        },
        keyPair_.duplicate());
    signingQueue->setWorkerCount(1);
    signingQueue->start();

    const ClientConnectionID connectionId;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_.emplace(connectionId, ClientConnectionState{selectedTransport, result.encoding, signingQueue});
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
            if (connectionState.signingQueue == nullptr) {
                continue;
            }

            const size_t queueSize = connectionState.signingQueue->size();
            if (queueSize == 0) {
                return connectionState.signingQueue->push(prepareOutboundMessage(message));
            }

            if (!selectionMade || queueSize < selectedQueueSize) {
                selectedQueue = connectionState.signingQueue;
                selectedQueueSize = queueSize;
                selectionMade = true;
            }
        }
    }

    return selectedQueue != nullptr && selectedQueue->push(prepareOutboundMessage(message));
}

bool RSPClientMessage::sendOnConnection(ClientConnectionID connectionId, const rsp::proto::RSPMessage& message) const {
    const auto selectedConnection = connectionState(connectionId);
    if (!selectedConnection.has_value() || selectedConnection->signingQueue == nullptr) {
        return false;
    }

    return selectedConnection->signingQueue->push(prepareOutboundMessage(message));
}

bool RSPClientMessage::tryDequeueMessage(rsp::proto::RSPMessage& message) const {
    return incomingMessages_ != nullptr && incomingMessages_->tryPop(message);
}

bool RSPClientMessage::waitAndDequeueMessage(rsp::proto::RSPMessage& message) const {
    if (incomingMessages_ == nullptr) {
        return false;
    }
    return incomingMessages_->blockingPop(message, std::chrono::milliseconds(50));
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

    if (removedConnection.signingQueue != nullptr) {
        removedConnection.signingQueue->stop();
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

    if (transportName == "memory") {
        return std::make_shared<rsp::transport::MemoryTransport>();
    }

    return nullptr;
}

rsp::proto::RSPMessage RSPClientMessage::prepareOutboundMessage(const rsp::proto::RSPMessage& message) const {
    rsp::proto::RSPMessage prepared = message;
    std::string nodeIdBytes(16, '\0');
    const uint64_t high = keyPair_.nodeID().high();
    const uint64_t low = keyPair_.nodeID().low();
    std::memcpy(nodeIdBytes.data(), &high, sizeof(high));
    std::memcpy(nodeIdBytes.data() + sizeof(high), &low, sizeof(low));
    prepared.mutable_source()->set_value(nodeIdBytes);

    if (!prepared.has_nonce()) {
        prepared.mutable_nonce()->set_value(randomMessageNonce());
    }

    return prepared;
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