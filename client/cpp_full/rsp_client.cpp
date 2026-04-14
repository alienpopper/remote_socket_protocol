#include "client/cpp_full/rsp_client.hpp"

#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/message_queue/mq_authn.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "common/transport/transport_memory.hpp"
#include "common/transport/transport_tcp.hpp"
#include "os/os_random.hpp"

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
        if (connectionState.encoding != nullptr) {
            connectionState.encoding->stop();
        }

        if (connectionState.signingQueue != nullptr) {
            connectionState.signingQueue->stop();
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
        connections_.emplace(connectionId, ClientConnectionState{selectedTransport, result.encoding, signingQueue});
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