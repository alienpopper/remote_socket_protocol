#include "client/cpp_full/rsp_client.hpp"

#include "common/base_types.hpp"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/message_queue/mq_authn.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "common/transport/transport_memory.hpp"
#include "common/transport/transport_tcp.hpp"
#include "os/os_random.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
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

rsp::proto::DateTime toProtoDateTime(const rsp::DateTime& dt) {
    rsp::proto::DateTime proto;
    proto.set_milliseconds_since_epoch(dt.millisecondsSinceEpoch());
    return proto;
}

std::string randomMessageNonce() {
    std::string nonce(16, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(nonce.data()), static_cast<uint32_t>(nonce.size()));
    return nonce;
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
        pingCv_.notify_all();
    }

    if (incomingMessages_ != nullptr) {
        incomingMessages_->stop();
    }

    stopNodeQueues();

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        removedConnections.swap(connections_);
    }

    for (auto& [_, connectionState] : removedConnections) {
        if (connectionState.reconnect != nullptr) {
            connectionState.reconnect->stopping.store(true);
            connectionState.reconnect->stopCondition.notify_all();
        }
        if (connectionState.transport != nullptr) {
            connectionState.transport->stop();
        }
        if (connectionState.reconnect != nullptr) {
            std::lock_guard<std::mutex> lock(connectionState.reconnect->threadMutex);
            if (connectionState.reconnect->thread.joinable()) {
                connectionState.reconnect->thread.join();
            }
        }
        if (connectionState.encoding != nullptr) {
            connectionState.encoding->stop();
        }
        if (connectionState.signingQueue != nullptr) {
            connectionState.signingQueue->stop();
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
        keyPair().duplicate(),
        bootId(),
        [&](const rsp::encoding::EncodingHandle& establishedEncoding) { finish(establishedEncoding, std::string()); },
        [&](const rsp::encoding::EncodingHandle& establishedEncoding) {
            if (establishedEncoding != nullptr) {
                establishedEncoding->stop();
            }
            finish(nullptr, "identity handshake failed");
        },
        [this](const rsp::NodeID& peerNodeId, const rsp::proto::Identity& identity) {
            rsp::proto::RSPMessage message;
            *message.mutable_source() = toProtoNodeId(peerNodeId);
            *message.add_identities() = identity;
            observeMessage(message);
        });
    authnQueue->setWorkerCount(1);
    authnQueue->start();

    const auto handshakeQueue = std::make_shared<rsp::message_queue::MessageQueueAsciiHandshakeClient>(
        incomingMessages_,
        keyPair().duplicate(),
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
            finish(nullptr, "client handshake failed");
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
                std::cerr << "RSP full client failed to enqueue signed message for transport" << std::endl;
            }
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            std::cerr << "RSP full client failed to sign outbound message: " << reason << std::endl;
        },
        keyPair().duplicate());
    signingQueue->setWorkerCount(1);
    signingQueue->start();

    const ClientConnectionID connectionId;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        ClientConnectionState state;
        state.transport = selectedTransport;
        state.encoding = result.encoding;
        state.signingQueue = signingQueue;
        state.reconnect = std::make_shared<ReconnectContext>();
        state.reconnect->transportSpec = transportSpec;
        state.reconnect->encodingType = encoding;
        state.reconnect->stopping.store(true); // will be set false by enableReconnect() below
        connections_.emplace(connectionId, std::move(state));
    }

    enableReconnect(connectionId);

    return connectionId;
}

bool RSPClient::send(const rsp::proto::RSPMessage& message) const {
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

bool RSPClient::sendOnConnection(ClientConnectionID connectionId, const rsp::proto::RSPMessage& message) const {
    const auto selectedConnection = connectionState(connectionId);
    if (!selectedConnection.has_value() || selectedConnection->signingQueue == nullptr) {
        return false;
    }

    return selectedConnection->signingQueue->push(prepareOutboundMessage(message));
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

    if (removedConnection.reconnect != nullptr) {
        removedConnection.reconnect->stopping.store(true);
        removedConnection.reconnect->stopCondition.notify_all();
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

void RSPClient::enableReconnect(ClientConnectionID connectionId,
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

    encoding->setDisconnectCallback(
        [this, connectionId, ctx](const rsp::NodeID&) {
            triggerReconnect(connectionId, ctx);
        });
}

void RSPClient::triggerReconnect(ClientConnectionID connectionId,
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

void RSPClient::doReconnect(ClientConnectionID connectionId,
                            std::shared_ptr<ReconnectContext> ctx) {
    rsp::transport::TransportHandle transport;
    std::string encodingType;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        const auto it = connections_.find(connectionId);
        if (it == connections_.end()) {
            ctx->inProgress.store(false);
            return;
        }
        transport = it->second.transport;
        encodingType = ctx->encodingType;
    }

    if (transport == nullptr || ctx->stopping.load()) {
        ctx->inProgress.store(false);
        return;
    }

    struct SingleConnectionTransport : rsp::transport::Transport {
        explicit SingleConnectionTransport(rsp::transport::ConnectionHandle c) : conn_(std::move(c)) {}
        rsp::transport::ConnectionHandle connect(const std::string&) override { return conn_; }
        rsp::transport::ConnectionHandle reconnect() override { return nullptr; }
        void stop() override { if (conn_) conn_->close(); }
        rsp::transport::ConnectionHandle connection() const override { return conn_; }
        rsp::transport::ConnectionHandle conn_;
    };

    rsp::encoding::EncodingHandle newEncoding;
    while (!ctx->stopping.load()) {
        const auto connection = transport->reconnect();
        if (connection == nullptr || ctx->stopping.load()) {
            ctx->inProgress.store(false);
            return;
        }

        struct ConnectResult {
            std::mutex mutex;
            std::condition_variable condition;
            bool complete = false;
            rsp::encoding::EncodingHandle encoding;
        } result;

        auto finish = [&](rsp::encoding::EncodingHandle enc) {
            std::lock_guard<std::mutex> lock(result.mutex);
            result.encoding = std::move(enc);
            result.complete = true;
            result.condition.notify_all();
        };

        const auto authnQueue = std::make_shared<rsp::message_queue::MessageQueueAuthN>(
            keyPair().duplicate(),
            bootId(),
            [&](const rsp::encoding::EncodingHandle& enc) { finish(enc); },
            [&](const rsp::encoding::EncodingHandle& enc) {
                if (enc != nullptr) {
                    enc->stop();
                }
                finish(nullptr);
            },
            [this](const rsp::NodeID& peerNodeId, const rsp::proto::Identity& identity) {
                rsp::proto::RSPMessage message;
                *message.mutable_source() = toProtoNodeId(peerNodeId);
                *message.add_identities() = identity;
                observeMessage(message);
            });
        authnQueue->setWorkerCount(1);
        authnQueue->start();

        const auto handshakeQueue = std::make_shared<rsp::message_queue::MessageQueueAsciiHandshakeClient>(
            incomingMessages_,
            keyPair().duplicate(),
            encodingType,
            [authnQueue, &finish](const rsp::encoding::EncodingHandle& enc) {
                if (authnQueue == nullptr || !authnQueue->push(enc)) {
                    if (enc != nullptr) {
                        enc->stop();
                    }
                    finish(nullptr);
                }
            },
            [&finish](const rsp::transport::TransportHandle&) {
                finish(nullptr);
            });
        handshakeQueue->setWorkerCount(1);
        handshakeQueue->start();

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

        if (result.encoding != nullptr) {
            newEncoding = result.encoding;
            break;
        }

        std::cerr << "[RSP full client] doReconnect: handshake failed, retrying" << std::endl;
        // Brief delay before retrying so we don't spin-hammer a recovering RM.
        {
            std::unique_lock<std::mutex> lock(ctx->threadMutex);
            ctx->stopCondition.wait_for(lock, std::chrono::milliseconds(500),
                                        [&ctx]() { return ctx->stopping.load(); });
        }
    }

    if (newEncoding == nullptr || ctx->stopping.load()) {
        ctx->inProgress.store(false);
        return;
    }

    if (!newEncoding->start()) {
        newEncoding->stop();
        ctx->inProgress.store(false);
        return;
    }

    const auto newSigningQueue = std::make_shared<rsp::MessageQueueSign>(
        [encodingHandle = newEncoding](rsp::proto::RSPMessage message) {
            const auto outgoing = encodingHandle == nullptr ? nullptr : encodingHandle->outgoingMessages();
            if (outgoing == nullptr || !outgoing->push(std::move(message))) {
                std::cerr << "RSP full client reconnect: failed to enqueue signed message" << std::endl;
            }
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            std::cerr << "RSP full client reconnect: failed to sign message: " << reason << std::endl;
        },
        keyPair().duplicate());
    newSigningQueue->setWorkerCount(1);
    newSigningQueue->start();

    rsp::encoding::EncodingHandle oldEncoding;
    rsp::MessageQueueHandle oldSigningQueue;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        const auto it = connections_.find(connectionId);
        if (it == connections_.end() || ctx->stopping.load()) {
            newEncoding->stop();
            newSigningQueue->stop();
            ctx->inProgress.store(false);
            return;
        }
        oldEncoding = it->second.encoding;
        oldSigningQueue = it->second.signingQueue;
        it->second.encoding = newEncoding;
        it->second.signingQueue = newSigningQueue;
    }

    if (oldSigningQueue != nullptr) {
        oldSigningQueue->stop();
    }
    if (oldEncoding != nullptr) {
        oldEncoding->stop();
    }

    newEncoding->setDisconnectCallback(
        [this, connectionId, ctx](const rsp::NodeID&) {
            triggerReconnect(connectionId, ctx);
        });

    if (ctx->onReconnected) {
        ctx->onReconnected(connectionId);
    }

    ctx->inProgress.store(false);
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

bool RSPClient::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_ping_reply()) {
        handlePingReply(message);
        return true;
    }
    return false;
}

bool RSPClient::ping(rsp::NodeID nodeId, uint32_t timeoutMs) {
    const std::string nonce = rsp::GUID().toString();
    const uint32_t sequence = [this, &nonce, &nodeId]() {
        std::lock_guard<std::mutex> lock(pingMutex_);
        const uint32_t next = nextPingSequence_++;
        pendingPings_.emplace(nonce, PendingPingState{nodeId, next, false});
        return next;
    }();

    rsp::proto::RSPMessage pingRequest;
    *pingRequest.mutable_destination() = toProtoNodeId(nodeId);
    pingRequest.mutable_ping_request()->mutable_nonce()->set_value(nonce);
    pingRequest.mutable_ping_request()->set_sequence(sequence);
    *pingRequest.mutable_ping_request()->mutable_time_sent() = toProtoDateTime(rsp::DateTime());

    if (!send(pingRequest)) {
        std::lock_guard<std::mutex> lock(pingMutex_);
        pendingPings_.erase(nonce);
        return false;
    }

    std::unique_lock<std::mutex> lock(pingMutex_);
    const bool replied = pingCv_.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMs),
        [this, &nonce]() {
            const auto it = pendingPings_.find(nonce);
            return stopping_ || (it != pendingPings_.end() && it->second.completed);
        });

    const bool success = replied && !stopping_ &&
                         pendingPings_.count(nonce) && pendingPings_.at(nonce).completed;
    pendingPings_.erase(nonce);
    return success;
}

void RSPClient::handlePingReply(const rsp::proto::RSPMessage& message) {
    if (!message.ping_reply().has_nonce()) {
        return;
    }
    const std::string& nonce = message.ping_reply().nonce().value();
    std::lock_guard<std::mutex> lock(pingMutex_);
    const auto it = pendingPings_.find(nonce);
    if (it == pendingPings_.end()) {
        return;
    }
    if (message.ping_reply().sequence() != it->second.sequence) {
        return;
    }
    it->second.completed = true;
    pingCv_.notify_all();
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

    if (transportName == "memory") {
        return std::make_shared<rsp::transport::MemoryTransport>();
    }

    return nullptr;
}

rsp::proto::RSPMessage RSPClient::prepareOutboundMessage(const rsp::proto::RSPMessage& message) const {
    rsp::proto::RSPMessage prepared = message;

    if (!prepared.has_nonce()) {
        prepared.mutable_nonce()->set_value(randomMessageNonce());
    }

    return prepared;
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