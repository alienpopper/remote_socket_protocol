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
    std::string bootIdBytes(16, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(bootIdBytes.data()), 16);
    bootId_.set_value(bootIdBytes);
}

int RSPClientMessage::run() const {
    return 0;
}

std::optional<RSPClientMessage::ClientConnectionID> RSPClientMessage::connectToResourceManager(const std::string& transport,
                                                                                const std::string& encoding) {
    std::string transportName;
    std::string transportParameters;
    if (!splitTransportSpec(transport, transportName, transportParameters)) {
        return std::nullopt;
    }

    const auto selectedTransport = createTransport(transportName);
    if (selectedTransport == nullptr) {
        return std::nullopt;
    }

    const rsp::transport::ConnectionHandle connection = selectedTransport->connect(transportParameters);
    if (connection == nullptr) {
        return std::nullopt;
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
        bootId_,
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
        return std::nullopt;
    }

    {
        std::unique_lock<std::mutex> lock(result.mutex);
        result.condition.wait(lock, [&result]() { return result.complete; });
    }

    handshakeQueue->stop();
    authnQueue->stop();

    if (result.encoding == nullptr) {
        selectedTransport->stop();
        return std::nullopt;
    }

    if (!result.encoding->start()) {
        result.encoding->stop();
        selectedTransport->stop();
        return std::nullopt;
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
        ClientConnectionState state;
        state.transport = selectedTransport;
        state.encoding = result.encoding;
        state.signingQueue = signingQueue;
        state.reconnect = nullptr;
        // Store for use by doReconnect
        state.reconnect = std::make_shared<ReconnectContext>();
        state.reconnect->transportSpec = transport;
        state.reconnect->encodingType = encoding;
        state.reconnect->stopping.store(true); // disabled until enableReconnect() is called
        connections_.emplace(connectionId, std::move(state));
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

const rsp::proto::Uuid& RSPClientMessage::bootId() const {
    return bootId_;
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

    // Abort any in-progress reconnect before stopping resources.
    // Setting stopping=true and calling transport->stop() causes
    // TcpTransport::reconnect() to return nullptr, which makes doReconnect exit.
    if (removedConnection.reconnect != nullptr) {
        removedConnection.reconnect->stopping.store(true);
    }

    if (removedConnection.transport != nullptr) {
        removedConnection.transport->stop();
    }

    if (removedConnection.reconnect != nullptr) {
        std::lock_guard<std::mutex> lock(removedConnection.reconnect->threadMutex);
        if (removedConnection.reconnect->thread.joinable()) {
            removedConnection.reconnect->thread.join();
        }
    }

    if (removedConnection.encoding != nullptr) {
        removedConnection.encoding->stop();
    }

    if (removedConnection.signingQueue != nullptr) {
        removedConnection.signingQueue->stop();
    }

    return true;
}

void RSPClientMessage::enableReconnect(ClientConnectionID connectionId,
                                       std::function<void(ClientConnectionID)> onReconnected) {
    std::shared_ptr<ReconnectContext> ctx;
    rsp::encoding::EncodingHandle encoding;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connections_.find(connectionId);
        if (it == connections_.end()) {
            return;
        }
        if (it->second.reconnect == nullptr) {
            it->second.reconnect = std::make_shared<ReconnectContext>();
        }
        it->second.reconnect->onReconnected = std::move(onReconnected);
        it->second.reconnect->stopping.store(false);
        ctx = it->second.reconnect;
        encoding = it->second.encoding;
    }

    const auto weakSelf = weak_from_this();
    encoding->setDisconnectCallback(
        [weakSelf, connectionId, ctx](const rsp::NodeID&) {
            const auto self = weakSelf.lock();
            if (self == nullptr) {
                return;
            }
            self->triggerReconnect(connectionId, ctx);
        });
}

void RSPClientMessage::triggerReconnect(ClientConnectionID connectionId,
                                        std::shared_ptr<ReconnectContext> ctx) {
    if (ctx == nullptr || ctx->stopping.load()) {
        return;
    }
    bool expected = false;
    if (!ctx->inProgress.compare_exchange_strong(expected, true)) {
        return;
    }

    std::lock_guard<std::mutex> lock(ctx->threadMutex);
    if (ctx->thread.joinable()) {
        ctx->thread.detach();
    }
    ctx->thread = std::thread([this, connectionId, ctx]() {
        doReconnect(connectionId, ctx);
    });
}

void RSPClientMessage::doReconnect(ClientConnectionID connectionId,
                                   std::shared_ptr<ReconnectContext> ctx) {
    rsp::transport::TransportHandle transport;
    std::string transportSpec;
    std::string encodingType;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        const auto it = connections_.find(connectionId);
        if (it == connections_.end()) {
            ctx->inProgress.store(false);
            return;
        }
        transport = it->second.transport;
        transportSpec = ctx->transportSpec;
        encodingType = ctx->encodingType;
    }

    if (transport == nullptr || ctx->stopping.load()) {
        ctx->inProgress.store(false);
        return;
    }

    // Block until TCP reconnects (TcpTransport::reconnect has exponential backoff).
    const auto connection = transport->reconnect();
    if (connection == nullptr || ctx->stopping.load()) {
        ctx->inProgress.store(false);
        return;
    }

    // Re-run ASCII handshake + AuthN pipeline to get a fresh encoding.
    struct ConnectResult {
        std::mutex mutex;
        std::condition_variable condition;
        bool complete = false;
        std::string error;
        rsp::encoding::EncodingHandle encoding;
    } result;

    auto finish = [&](rsp::encoding::EncodingHandle enc, std::string error) {
        std::lock_guard<std::mutex> lock(result.mutex);
        result.encoding = std::move(enc);
        result.error = std::move(error);
        result.complete = true;
        result.condition.notify_all();
    };

    const auto authnQueue = std::make_shared<rsp::message_queue::MessageQueueAuthN>(
        keyPair_.duplicate(),
        bootId_,
        [&](const rsp::encoding::EncodingHandle& enc) { finish(enc, {}); },
        [&](const rsp::encoding::EncodingHandle& enc) {
            if (enc != nullptr) {
                enc->stop();
            }
            finish(nullptr, "identity handshake failed");
        },
        [](const rsp::NodeID&, const rsp::proto::Identity&) {});
    authnQueue->setWorkerCount(1);
    authnQueue->start();

    const auto handshakeQueue = std::make_shared<rsp::message_queue::MessageQueueAsciiHandshakeClient>(
        incomingMessages_,
        keyPair_.duplicate(),
        encodingType,
        [authnQueue, &finish](const rsp::encoding::EncodingHandle& enc) {
            if (authnQueue == nullptr || !authnQueue->push(enc)) {
                if (enc != nullptr) {
                    enc->stop();
                }
                finish(nullptr, "failed to enqueue encoding for identity handshake");
            }
        },
        [&finish](const rsp::transport::TransportHandle&) {
            finish(nullptr, "ASCII handshake failed");
        });
    handshakeQueue->setWorkerCount(1);
    handshakeQueue->start();

    // Push the reconnected transport through the handshake pipeline.
    // We wrap the existing transport in a minimal TransportHandle that presents
    // the new connection to the handshake queue.
    struct SingleConnectionTransport : rsp::transport::Transport {
        explicit SingleConnectionTransport(rsp::transport::ConnectionHandle c) : conn_(std::move(c)) {}
        rsp::transport::ConnectionHandle connect(const std::string&) override { return conn_; }
        rsp::transport::ConnectionHandle reconnect() override { return nullptr; }
        void stop() override { if (conn_) conn_->close(); }
        rsp::transport::ConnectionHandle connection() const override { return conn_; }
        rsp::transport::ConnectionHandle conn_;
    };
    const auto wrapTransport = std::make_shared<SingleConnectionTransport>(connection);

    if (!handshakeQueue->push(wrapTransport)) {
        handshakeQueue->stop();
        authnQueue->stop();
        ctx->inProgress.store(false);
        return;
    }

    {
        std::unique_lock<std::mutex> lock(result.mutex);
        result.condition.wait(lock, [&result]() { return result.complete; });
    }
    handshakeQueue->stop();
    authnQueue->stop();

    if (result.encoding == nullptr || ctx->stopping.load()) {
        ctx->inProgress.store(false);
        return;
    }

    if (!result.encoding->start()) {
        result.encoding->stop();
        ctx->inProgress.store(false);
        return;
    }

    const auto newSigningQueue = std::make_shared<rsp::MessageQueueSign>(
        [encodingHandle = result.encoding](rsp::proto::RSPMessage message) {
            const auto outgoing = encodingHandle == nullptr ? nullptr : encodingHandle->outgoingMessages();
            if (outgoing == nullptr || !outgoing->push(std::move(message))) {
                std::cerr << "RSP client reconnect: failed to enqueue signed message" << std::endl;
            }
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            std::cerr << "RSP client reconnect: failed to sign message: " << reason << std::endl;
        },
        keyPair_.duplicate());
    newSigningQueue->setWorkerCount(1);
    newSigningQueue->start();

    // Swap in the new encoding and signing queue atomically.
    rsp::encoding::EncodingHandle oldEncoding;
    rsp::MessageQueueHandle oldSigningQueue;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        const auto it = connections_.find(connectionId);
        if (it == connections_.end() || ctx->stopping.load()) {
            result.encoding->stop();
            newSigningQueue->stop();
            ctx->inProgress.store(false);
            return;
        }
        oldEncoding = it->second.encoding;
        oldSigningQueue = it->second.signingQueue;
        it->second.encoding = result.encoding;
        it->second.signingQueue = newSigningQueue;
    }

    if (oldSigningQueue != nullptr) {
        oldSigningQueue->stop();
    }
    if (oldEncoding != nullptr) {
        oldEncoding->stop();
    }

    // Set disconnect callback on the new encoding so future drops also reconnect.
    const auto weakSelf = weak_from_this();
    result.encoding->setDisconnectCallback(
        [weakSelf, connectionId, ctx](const rsp::NodeID&) {
            const auto self = weakSelf.lock();
            if (self == nullptr) {
                return;
            }
            self->triggerReconnect(connectionId, ctx);
        });

    if (ctx->onReconnected) {
        ctx->onReconnected(connectionId);
    }

    ctx->inProgress.store(false);
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