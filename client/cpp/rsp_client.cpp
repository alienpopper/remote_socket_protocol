#include "client/cpp/rsp_client.hpp"

#include "common/base_types.hpp"
#include "common/endorsement/endorsement.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "common/ping_trace.hpp"

#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <optional>
#include <thread>
#include <utility>

namespace rsp::client {

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

rsp::proto::DateTime toProtoDateTime(const rsp::DateTime& dateTime) {
    rsp::proto::DateTime protoDateTime;
    protoDateTime.set_milliseconds_since_epoch(dateTime.millisecondsSinceEpoch());
    return protoDateTime;
}

void recordClientDequeueEvent(const rsp::proto::RSPMessage& message, const rsp::NodeID& localNodeId) {
    if (!rsp::ping_trace::isEnabled()) {
        return;
    }

    const std::string localNodeIdValue = toProtoNodeId(localNodeId).value();
    if (message.has_ping_request() && message.has_destination() && message.destination().value() == localNodeIdValue) {
        rsp::ping_trace::recordForMessage(message, "destination_client_poll_dequeue");
    } else if (message.has_ping_reply() && message.has_destination() && message.destination().value() == localNodeIdValue) {
        rsp::ping_trace::recordForMessage(message, "source_client_poll_dequeue");
    }
}

void recordNodeInputEnqueueEvent(const rsp::proto::RSPMessage& message, const rsp::NodeID& localNodeId) {
    if (!rsp::ping_trace::isEnabled()) {
        return;
    }

    const std::string localNodeIdValue = toProtoNodeId(localNodeId).value();
    if (message.has_ping_request() && message.has_destination() && message.destination().value() == localNodeIdValue) {
        rsp::ping_trace::recordForMessage(message, "destination_node_input_enqueued");
    } else if (message.has_ping_reply() && message.has_destination() && message.destination().value() == localNodeIdValue) {
        rsp::ping_trace::recordForMessage(message, "source_node_input_enqueued");
    }
}

bool socketRepliesMatch(const rsp::proto::SocketReply& left, const rsp::proto::SocketReply& right) {
    return left.SerializeAsString() == right.SerializeAsString();
}

bool sendAllToSocket(rsp::os::SocketHandle socketHandle, const uint8_t* data, std::size_t length) {
    std::size_t bytesSent = 0;
    while (bytesSent < length) {
        const int result = rsp::os::sendSocket(socketHandle,
                                               data + bytesSent,
                                               static_cast<uint32_t>(length - bytesSent));
        if (result <= 0) {
            return false;
        }

        bytesSent += static_cast<std::size_t>(result);
    }

    return true;
}

void removeBufferedReply(std::deque<rsp::proto::SocketReply>& pendingReplies,
                         const rsp::proto::SocketReply& reply) {
    for (auto iterator = pendingReplies.begin(); iterator != pendingReplies.end(); ++iterator) {
        if (socketRepliesMatch(*iterator, reply)) {
            pendingReplies.erase(iterator);
            return;
        }
    }
}

}  // namespace

RSPClient::Ptr RSPClient::create() {
    return Ptr(new RSPClient(KeyPair::generateP256()));
}

RSPClient::Ptr RSPClient::create(KeyPair keyPair) {
    return Ptr(new RSPClient(std::move(keyPair)));
}

RSPClient::RSPClient(KeyPair keyPair)
        : rsp::RSPNode(keyPair.duplicate()),
            messageClient_(RSPClientMessage::create(std::move(keyPair))) {
    receiveThread_ = std::thread([this]() { receiveLoop(); });
}

RSPClient::~RSPClient() {
    stopNativeListenBridges();
    stopNativeSocketBridges();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stopping_ = true;
    }
    stateChanged_.notify_all();

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    stopNodeQueues();
}

int RSPClient::run() const {
    return 0;
}

bool RSPClient::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_ping_reply()) {
        handlePingReply(message);
        return true;
    }

    if (message.has_endorsement_done()) {
        handleEndorsementDone(message);
        return true;
    }

    if (message.has_socket_reply()) {
        handleSocketReply(message);
        return true;
    }

    if (message.has_resource_advertisement()) {
        handleResourceAdvertisement(message);
        return true;
    }

    return false;
}

void RSPClient::handleOutputMessage(rsp::proto::RSPMessage message) {
    const bool sent = messageClient_->send(message);
    if (sent && rsp::ping_trace::isEnabled() && message.has_ping_reply()) {
        rsp::ping_trace::recordForMessage(message, "destination_reply_send_enqueued");
    }

    if (!sent) {
        std::cerr << "RSPClient failed to send message produced by node handler" << std::endl;
    }
}

RSPClient::ClientConnectionID RSPClient::connectToResourceManager(const std::string& transport,
                                                                  const std::string& encoding) {
    return messageClient_->connectToResourceManager(transport, encoding);
}

bool RSPClient::hasConnections() const {
    return messageClient_->hasConnections();
}

bool RSPClient::hasConnection(ClientConnectionID connectionId) const {
    return messageClient_->hasConnection(connectionId);
}

std::size_t RSPClient::connectionCount() const {
    return messageClient_->connectionCount();
}

std::vector<RSPClient::ClientConnectionID> RSPClient::connectionIds() const {
    return messageClient_->connectionIds();
}

bool RSPClient::removeConnection(ClientConnectionID connectionId) {
    const auto peerNodeId = messageClient_->peerNodeID(connectionId);
    if (peerNodeId.has_value()) {
        stopNativeListenBridgesForNode(*peerNodeId);
        stopNativeSocketBridgesForNode(*peerNodeId);
    }

    return messageClient_->removeConnection(connectionId);
}

std::optional<rsp::NodeID> RSPClient::peerNodeID(ClientConnectionID connectionId) const {
    return messageClient_->peerNodeID(connectionId);
}

bool RSPClient::ping(rsp::NodeID nodeId) {
    const std::string nonce = rsp::GUID().toString();
    rsp::ping_trace::start(nonce);
    rsp::ping_trace::record(nonce, "source_ping_call_start");
    const uint32_t sequence = [this, &nonce, &nodeId]() {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const uint32_t nextSequence = nextPingSequence_++;
        pendingPings_.emplace(nonce, PendingPingState{nodeId, nextSequence, false});
        return nextSequence;
    }();

    rsp::proto::RSPMessage pingRequest;
    *pingRequest.mutable_destination() = toProtoNodeId(nodeId);
    pingRequest.mutable_ping_request()->mutable_nonce()->set_value(nonce);
    pingRequest.mutable_ping_request()->set_sequence(sequence);
    *pingRequest.mutable_ping_request()->mutable_time_sent() = toProtoDateTime(rsp::DateTime());

    if (!messageClient_->send(pingRequest)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingPings_.erase(nonce);
        return false;
    }
    rsp::ping_trace::record(nonce, "source_request_send_enqueued");

    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool replied = stateChanged_.wait_for(
        lock,
        std::chrono::seconds(5),
        [this, &nonce]() {
            const auto iterator = pendingPings_.find(nonce);
            return stopping_ || (iterator != pendingPings_.end() && iterator->second.completed);
        });

    if (!replied || stopping_) {
        pendingPings_.erase(nonce);
        return false;
    }

    rsp::ping_trace::record(nonce, "source_ping_wait_completed");
    pendingPings_.erase(nonce);
    return true;
}

std::optional<rsp::proto::EndorsementDone> RSPClient::beginEndorsementRequest(
    rsp::NodeID nodeId,
    const rsp::GUID& endorsementType,
    const rsp::Buffer& endorsementValue) {
    const std::string pendingKey = toProtoNodeId(nodeId).value();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (pendingEndorsements_.find(pendingKey) != pendingEndorsements_.end()) {
            return std::nullopt;
        }

        pendingEndorsements_.emplace(pendingKey, PendingEndorsementState{nodeId, false, std::nullopt});
    }

    rsp::DateTime requestedValidUntil;
    requestedValidUntil += DAYS(1);
    const rsp::Endorsement requested = rsp::Endorsement::createSigned(
        keyPair(),
        keyPair().nodeID(),
        endorsementType,
        endorsementValue,
        requestedValidUntil);

    const rsp::proto::Endorsement requestedMessage = requested.toProto();

    bool repairedUnknownIdentity = false;
    while (true) {
        if (!sendBeginEndorsementRequestMessage(nodeId, requestedMessage)) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            pendingEndorsements_.erase(pendingKey);
            return std::nullopt;
        }

        auto reply = waitForPendingEndorsement(pendingKey);
        if (!reply.has_value()) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            pendingEndorsements_.erase(pendingKey);
            return std::nullopt;
        }

        if (!repairedUnknownIdentity && reply->status() == rsp::proto::ENDORSEMENT_UNKNOWN_IDENTITY) {
            if (!sendIdentity(nodeId)) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                pendingEndorsements_.erase(pendingKey);
                return std::nullopt;
            }

            repairedUnknownIdentity = true;
            continue;
        }

        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingEndorsements_.erase(pendingKey);
        return reply;
    }
}

bool RSPClient::sendIdentity(rsp::NodeID nodeId) {
    rsp::proto::RSPMessage identityMessage;
    *identityMessage.mutable_destination() = toProtoNodeId(nodeId);
    *identityMessage.mutable_identity()->mutable_public_key() = keyPair().publicKey();
    return messageClient_ != nullptr && messageClient_->send(identityMessage);
}

bool RSPClient::sendBeginEndorsementRequestMessage(rsp::NodeID nodeId,
                                                   const rsp::proto::Endorsement& requestedMessage) {
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    *request.mutable_begin_endorsement_request()->mutable_requested_values() = requestedMessage;
    return messageClient_ != nullptr && messageClient_->send(request);
}

std::optional<rsp::proto::EndorsementDone> RSPClient::waitForPendingEndorsement(const std::string& pendingKey) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool replied = stateChanged_.wait_for(lock, std::chrono::seconds(5), [this, &pendingKey]() {
        const auto iterator = pendingEndorsements_.find(pendingKey);
        return stopping_ || (iterator != pendingEndorsements_.end() && iterator->second.completed);
    });
    const auto iterator = pendingEndorsements_.find(pendingKey);
    if (!replied || stopping_ || iterator == pendingEndorsements_.end()) {
        return std::nullopt;
    }

    auto reply = iterator->second.reply;
    iterator->second.completed = false;
    iterator->second.reply.reset();
    return reply;
}

bool RSPClient::queryResources(rsp::NodeID nodeId, const std::string& query, uint32_t maxRecords) {
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    auto* resourceQuery = request.mutable_resource_query();
    if (!query.empty()) {
        resourceQuery->set_query(query);
    }
    if (maxRecords > 0) {
        resourceQuery->set_max_records(maxRecords);
    }

    return messageClient_->send(request);
}

std::optional<rsp::proto::SocketReply> RSPClient::connectTCPEx(rsp::NodeID nodeId,
                                                                  const std::string& hostPort,
                                                                  uint32_t timeoutMilliseconds,
                                                                  uint32_t retries,
                                                                  uint32_t retryMilliseconds,
                                                                  bool asyncData,
                                                                  bool shareSocket,
                                                                  bool useSocket) {
    const rsp::GUID socketId;
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    request.mutable_connect_tcp_request()->set_host_port(hostPort);
    *request.mutable_connect_tcp_request()->mutable_socket_number() = toProtoSocketId(socketId);
    request.mutable_connect_tcp_request()->set_use_socket(useSocket);
    if (timeoutMilliseconds > 0) {
        request.mutable_connect_tcp_request()->set_timeout_ms(timeoutMilliseconds);
    }
    if (retries > 0) {
        request.mutable_connect_tcp_request()->set_retries(retries);
    }
    if (retryMilliseconds > 0) {
        request.mutable_connect_tcp_request()->set_retry_ms(retryMilliseconds);
    }
    if (asyncData) {
        request.mutable_connect_tcp_request()->set_async_data(true);
    }
    if (shareSocket) {
        request.mutable_connect_tcp_request()->set_share_socket(true);
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingConnects_[socketId] = PendingConnectState{};
    }

    if (!messageClient_->send(request)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingConnects_.erase(socketId);
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool replied = stateChanged_.wait_for(lock, std::chrono::seconds(5), [this, &socketId]() {
        const auto iterator = pendingConnects_.find(socketId);
        return stopping_ || (iterator != pendingConnects_.end() && iterator->second.completed);
    });
    const auto iterator = pendingConnects_.find(socketId);
    if (!replied || stopping_ || iterator == pendingConnects_.end()) {
        pendingConnects_.erase(socketId);
        return std::nullopt;
    }

    auto reply = iterator->second.reply;
    pendingConnects_.erase(iterator);
    if (reply.has_value() && reply->error() == rsp::proto::SUCCESS) {
        if (!reply->has_socket_id()) {
            *reply->mutable_socket_id() = toProtoSocketId(socketId);
        }
        socketRoutes_[socketId] = nodeId;
    }

    return reply;
}

std::optional<rsp::GUID> RSPClient::connectTCP(rsp::NodeID nodeId,
                                               const std::string& hostPort,
                                               uint32_t timeoutMilliseconds,
                                               uint32_t retries,
                                               uint32_t retryMilliseconds,
                                               bool asyncData,
                                               bool shareSocket,
                                               bool useSocket) {
    const auto reply = connectTCPEx(nodeId,
                                       hostPort,
                                       timeoutMilliseconds,
                                       retries,
                                       retryMilliseconds,
                                       asyncData,
                                       shareSocket,
                                       useSocket);
    if (!reply.has_value() || reply->error() != rsp::proto::SUCCESS || !reply->has_socket_id()) {
        return std::nullopt;
    }

    return fromProtoSocketId(reply->socket_id());
}

std::optional<rsp::proto::SocketReply> RSPClient::listenTCPEx(rsp::NodeID nodeId,
                                                              const std::string& hostPort,
                                                              uint32_t timeoutMilliseconds,
                                                              bool asyncAccept,
                                                              bool shareListeningSocket,
                                                              bool shareChildSockets,
                                                              bool childrenUseSocket,
                                                              bool childrenAsyncData) {
    const rsp::GUID socketId;
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    request.mutable_listen_tcp_request()->set_host_port(hostPort);
    *request.mutable_listen_tcp_request()->mutable_socket_number() = toProtoSocketId(socketId);
    if (timeoutMilliseconds > 0) {
        request.mutable_listen_tcp_request()->set_timeout_ms(timeoutMilliseconds);
    }
    if (asyncAccept) {
        request.mutable_listen_tcp_request()->set_async_accept(true);
    }
    if (shareListeningSocket) {
        request.mutable_listen_tcp_request()->set_share_listening_socket(true);
    }
    if (shareChildSockets) {
        request.mutable_listen_tcp_request()->set_share_child_sockets(true);
    }
    if (childrenUseSocket) {
        request.mutable_listen_tcp_request()->set_children_use_socket(true);
    }
    if (childrenAsyncData) {
        request.mutable_listen_tcp_request()->set_children_async_data(true);
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingListens_[socketId] = PendingConnectState{};
    }

    if (!messageClient_->send(request)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingListens_.erase(socketId);
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool replied = stateChanged_.wait_for(lock, std::chrono::seconds(5), [this, &socketId]() {
        const auto iterator = pendingListens_.find(socketId);
        return stopping_ || (iterator != pendingListens_.end() && iterator->second.completed);
    });
    const auto iterator = pendingListens_.find(socketId);
    if (!replied || stopping_ || iterator == pendingListens_.end()) {
        pendingListens_.erase(socketId);
        return std::nullopt;
    }

    auto reply = iterator->second.reply;
    pendingListens_.erase(iterator);
    if (reply.has_value() && reply->error() == rsp::proto::SUCCESS) {
        if (!reply->has_socket_id()) {
            *reply->mutable_socket_id() = toProtoSocketId(socketId);
        }
        socketRoutes_[socketId] = nodeId;
    }

    return reply;
}

std::optional<rsp::GUID> RSPClient::listenTCP(rsp::NodeID nodeId,
                                              const std::string& hostPort,
                                              uint32_t timeoutMilliseconds,
                                              bool asyncAccept,
                                              bool shareListeningSocket,
                                              bool shareChildSockets,
                                              bool childrenUseSocket,
                                              bool childrenAsyncData) {
    const auto reply = listenTCPEx(nodeId,
                                   hostPort,
                                   timeoutMilliseconds,
                                   asyncAccept,
                                   shareListeningSocket,
                                   shareChildSockets,
                                   childrenUseSocket,
                                   childrenAsyncData);
    if (!reply.has_value() || reply->error() != rsp::proto::SUCCESS || !reply->has_socket_id()) {
        return std::nullopt;
    }

    return fromProtoSocketId(reply->socket_id());
}

std::optional<rsp::os::SocketHandle> RSPClient::listenTCPSocket(rsp::NodeID nodeId,
                                                                const std::string& hostPort,
                                                                uint32_t timeoutMilliseconds) {
    const auto listenSocketId = listenTCP(nodeId, hostPort, timeoutMilliseconds);
    if (!listenSocketId.has_value()) {
        return std::nullopt;
    }

    rsp::os::SocketHandle listenerSocket = rsp::os::invalidSocket();
    std::string localEndpoint;
    if (!rsp::os::createLocalListenerSocket(listenerSocket, localEndpoint)) {
        socketClose(*listenSocketId);
        return std::nullopt;
    }

    auto bridgeState = std::make_shared<NativeListenBridgeState>();
    bridgeState->listenSocketId = *listenSocketId;
    bridgeState->nodeId = nodeId;
    bridgeState->localEndpoint = std::move(localEndpoint);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        nativeListenBridges_[*listenSocketId] = bridgeState;
    }

    bridgeState->worker = std::thread([weakSelf = weak_from_this(), bridgeState]() {
        if (const auto self = weakSelf.lock()) {
            self->runNativeListenSocketBridge(bridgeState);
        }
    });

    return listenerSocket;
}

std::optional<rsp::proto::SocketReply> RSPClient::acceptTCPEx(const rsp::GUID& listenSocketId,
                                                              const std::optional<rsp::GUID>& newSocketId,
                                                              uint32_t timeoutMilliseconds,
                                                              bool shareChildSocket,
                                                              bool childUseSocket,
                                                              bool childAsyncData) {
    rsp::NodeID destination;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto iterator = socketRoutes_.find(listenSocketId);
        if (iterator == socketRoutes_.end()) {
            return std::nullopt;
        }

        destination = iterator->second;
        awaitedSocketReplies_.insert(listenSocketId);
    }

    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(destination);
    *request.mutable_accept_tcp()->mutable_listen_socket_number() = toProtoSocketId(listenSocketId);
    if (newSocketId.has_value()) {
        *request.mutable_accept_tcp()->mutable_new_socket_number() = toProtoSocketId(*newSocketId);
    }
    if (timeoutMilliseconds > 0) {
        request.mutable_accept_tcp()->set_timeout_ms(timeoutMilliseconds);
    }
    if (shareChildSocket) {
        request.mutable_accept_tcp()->set_share_child_socket(true);
    }
    if (childUseSocket) {
        request.mutable_accept_tcp()->set_child_use_socket(true);
    }
    if (childAsyncData) {
        request.mutable_accept_tcp()->set_child_async_data(true);
    }

    if (!messageClient_->send(request)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        awaitedSocketReplies_.erase(listenSocketId);
        return std::nullopt;
    }

    auto reply = waitForSocketReply(listenSocketId);
    if (reply.has_value() &&
        (reply->error() == rsp::proto::SUCCESS || reply->error() == rsp::proto::NEW_CONNECTION) &&
        reply->has_new_socket_id()) {
        const auto acceptedSocketId = fromProtoSocketId(reply->new_socket_id());
        if (acceptedSocketId.has_value()) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            socketRoutes_[*acceptedSocketId] = destination;
        }
    }

    return reply;
}

std::optional<rsp::GUID> RSPClient::acceptTCP(const rsp::GUID& listenSocketId,
                                              const std::optional<rsp::GUID>& newSocketId,
                                              uint32_t timeoutMilliseconds,
                                              bool shareChildSocket,
                                              bool childUseSocket,
                                              bool childAsyncData) {
    const auto reply = acceptTCPEx(listenSocketId,
                                   newSocketId,
                                   timeoutMilliseconds,
                                   shareChildSocket,
                                   childUseSocket,
                                   childAsyncData);
    if (!reply.has_value() ||
        (reply->error() != rsp::proto::SUCCESS && reply->error() != rsp::proto::NEW_CONNECTION) ||
        !reply->has_new_socket_id()) {
        return std::nullopt;
    }

    return fromProtoSocketId(reply->new_socket_id());
}

std::optional<rsp::os::SocketHandle> RSPClient::acceptTCPSocket(const rsp::GUID& listenSocketId,
                                                                const std::optional<rsp::GUID>& newSocketId,
                                                                uint32_t timeoutMilliseconds) {
    rsp::os::SocketHandle applicationSocket = rsp::os::invalidSocket();
    rsp::os::SocketHandle bridgeSocket = rsp::os::invalidSocket();
    if (!rsp::os::createSocketPair(applicationSocket, bridgeSocket)) {
        return std::nullopt;
    }

    const auto socketId = acceptTCP(listenSocketId,
                                    newSocketId,
                                    timeoutMilliseconds,
                                    false,
                                    false,
                                    true);
    if (!socketId.has_value()) {
        rsp::os::closeSocket(applicationSocket);
        rsp::os::closeSocket(bridgeSocket);
        return std::nullopt;
    }

    const auto bridgeState = attachNativeSocketBridge(*socketId, bridgeSocket);
    startNativeSocketBridgeWorker(*socketId, bridgeState);

    return applicationSocket;
}

std::optional<rsp::os::SocketHandle> RSPClient::connectTCPSocket(rsp::NodeID nodeId,
                                                                 const std::string& hostPort,
                                                                 uint32_t timeoutMilliseconds,
                                                                 uint32_t retries,
                                                                 uint32_t retryMilliseconds) {
    rsp::os::SocketHandle applicationSocket = rsp::os::invalidSocket();
    rsp::os::SocketHandle bridgeSocket = rsp::os::invalidSocket();
    if (!rsp::os::createSocketPair(applicationSocket, bridgeSocket)) {
        return std::nullopt;
    }

    const auto socketId = connectTCP(nodeId,
                                     hostPort,
                                     timeoutMilliseconds,
                                     retries,
                                     retryMilliseconds,
                                     true,
                                     false,
                                     false);
    if (!socketId.has_value()) {
        rsp::os::closeSocket(applicationSocket);
        rsp::os::closeSocket(bridgeSocket);
        return std::nullopt;
    }

    const auto bridgeState = attachNativeSocketBridge(*socketId, bridgeSocket);
    startNativeSocketBridgeWorker(*socketId, bridgeState);

    return applicationSocket;
}

std::shared_ptr<RSPClient::NativeSocketBridgeState> RSPClient::attachNativeSocketBridge(
    const rsp::GUID& socketId,
    rsp::os::SocketHandle bridgeSocket) {
    auto bridgeState = std::make_shared<NativeSocketBridgeState>();
    bridgeState->bridgeSocket = bridgeSocket;

    std::deque<rsp::proto::SocketReply> bufferedReplies;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        nativeSocketBridges_[socketId] = bridgeState;
        const auto queuedReplies = socketReplyQueues_.find(socketId);
        if (queuedReplies != socketReplyQueues_.end()) {
            bufferedReplies = std::move(queuedReplies->second);
            socketReplyQueues_.erase(queuedReplies);
            for (const auto& reply : bufferedReplies) {
                removeBufferedReply(pendingSocketReplies_, reply);
            }
        }
    }

    for (const auto& reply : bufferedReplies) {
        if (reply.error() == rsp::proto::SOCKET_DATA && reply.has_data()) {
            if (!sendAllToSocket(bridgeState->bridgeSocket,
                                 reinterpret_cast<const uint8_t*>(reply.data().data()),
                                 reply.data().size())) {
                bridgeState->stopping.store(true);
                rsp::os::closeSocket(bridgeState->bridgeSocket);
                break;
            }

            continue;
        }

        bridgeState->remoteClosed.store(true);
        bridgeState->stopping.store(true);
        rsp::os::closeSocket(bridgeState->bridgeSocket);
        break;
    }

    return bridgeState;
}

void RSPClient::startNativeSocketBridgeWorker(const rsp::GUID& socketId,
                                              const std::shared_ptr<NativeSocketBridgeState>& bridgeState) {
    bridgeState->worker = std::thread([weakSelf = weak_from_this(), socketId, bridgeState]() {
        if (const auto self = weakSelf.lock()) {
            self->runNativeSocketBridge(socketId, bridgeState);
        } else {
            rsp::os::closeSocket(bridgeState->bridgeSocket);
        }
    });
}

bool RSPClient::socketSend(const rsp::GUID& socketId, const std::string& data) {
    rsp::NodeID destination;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto iterator = socketRoutes_.find(socketId);
        if (iterator == socketRoutes_.end()) {
            return false;
        }

        destination = iterator->second;
        awaitedSocketReplies_.insert(socketId);
    }

    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(destination);
    *request.mutable_socket_send()->mutable_socket_number() = toProtoSocketId(socketId);
    request.mutable_socket_send()->set_data(data);

    if (!messageClient_->send(request)) {
        return false;
    }

    const auto reply = waitForSocketReply(socketId);
    if (!reply.has_value()) {
        return false;
    }

    return reply->error() == rsp::proto::SUCCESS;
}

std::optional<std::string> RSPClient::socketRecv(const rsp::GUID& socketId,
                                                 uint32_t maxBytes,
                                                 uint32_t waitMilliseconds) {
    const auto reply = socketRecvEx(socketId, maxBytes, waitMilliseconds);
    if (!reply.has_value()) {
        return std::nullopt;
    }

    if (reply->error() != rsp::proto::SOCKET_DATA && reply->error() != rsp::proto::SUCCESS) {
        return std::nullopt;
    }

    return reply->has_data() ? std::optional<std::string>(reply->data()) : std::optional<std::string>(std::string());
}

std::optional<rsp::proto::SocketReply> RSPClient::socketRecvEx(const rsp::GUID& socketId,
                                                                  uint32_t maxBytes,
                                                                  uint32_t waitMilliseconds) {
    rsp::NodeID destination;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto iterator = socketRoutes_.find(socketId);
        if (iterator == socketRoutes_.end()) {
            return std::nullopt;
        }

        destination = iterator->second;
        awaitedSocketReplies_.insert(socketId);
    }

    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(destination);
    *request.mutable_socket_recv()->mutable_socket_number() = toProtoSocketId(socketId);
    request.mutable_socket_recv()->set_max_bytes(maxBytes);
    if (waitMilliseconds > 0) {
        request.mutable_socket_recv()->set_wait_ms(waitMilliseconds);
    }

    if (!messageClient_->send(request)) {
        return std::nullopt;
    }

    return waitForSocketReply(socketId);
}

std::optional<rsp::proto::SocketReply> RSPClient::waitForSocketReply(const rsp::GUID& socketId) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool replied = stateChanged_.wait_for(lock, std::chrono::seconds(5), [this, &socketId]() {
        const auto iterator = socketReplyQueues_.find(socketId);
        return stopping_ || (iterator != socketReplyQueues_.end() && !iterator->second.empty());
    });
    const auto iterator = socketReplyQueues_.find(socketId);
    if (!replied || stopping_ || iterator == socketReplyQueues_.end() || iterator->second.empty()) {
        awaitedSocketReplies_.erase(socketId);
        return std::nullopt;
    }

    const rsp::proto::SocketReply reply = iterator->second.front();
    iterator->second.pop_front();
    awaitedSocketReplies_.erase(socketId);
    for (auto globalIterator = pendingSocketReplies_.begin(); globalIterator != pendingSocketReplies_.end(); ++globalIterator) {
        if (socketRepliesMatch(*globalIterator, reply)) {
            pendingSocketReplies_.erase(globalIterator);
            break;
        }
    }
    if (iterator->second.empty()) {
        socketReplyQueues_.erase(iterator);
    }
    return reply;
}

bool RSPClient::socketClose(const rsp::GUID& socketId) {
    rsp::NodeID destination;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto iterator = socketRoutes_.find(socketId);
        if (iterator == socketRoutes_.end()) {
            return false;
        }

        destination = iterator->second;
        awaitedSocketReplies_.insert(socketId);
    }

    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(destination);
    *request.mutable_socket_close()->mutable_socket_number() = toProtoSocketId(socketId);

    if (!messageClient_->send(request)) {
        socketRoutes_.erase(socketId);
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto reply = waitForSocketReply(socketId);
        if (!reply.has_value()) {
            socketRoutes_.erase(socketId);
            return true;
        }

        if (reply->error() == rsp::proto::SUCCESS || reply->error() == rsp::proto::SOCKET_CLOSED) {
            socketRoutes_.erase(socketId);
            return true;
        }

        if (reply->error() != rsp::proto::SOCKET_DATA &&
            reply->error() != rsp::proto::ASYNC_SOCKET &&
            reply->error() != rsp::proto::NEW_CONNECTION) {
            return false;
        }
    }

    socketRoutes_.erase(socketId);
    return true;
}

bool RSPClient::tryDequeueSocketReply(rsp::proto::SocketReply& reply) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (pendingSocketReplies_.empty()) {
        return false;
    }

    reply = pendingSocketReplies_.front();
    pendingSocketReplies_.pop_front();
    return true;
}

std::size_t RSPClient::pendingSocketReplyCount() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return pendingSocketReplies_.size();
}

bool RSPClient::tryDequeueResourceAdvertisement(rsp::proto::ResourceAdvertisement& advertisement) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (pendingResourceAdvertisements_.empty()) {
        return false;
    }

    advertisement = pendingResourceAdvertisements_.front();
    pendingResourceAdvertisements_.pop_front();
    return true;
}

std::size_t RSPClient::pendingResourceAdvertisementCount() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return pendingResourceAdvertisements_.size();
}

void RSPClient::registerSocketRoute(const rsp::GUID& socketId, rsp::NodeID nodeId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    socketRoutes_[socketId] = nodeId;
}

void RSPClient::receiveLoop() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (stopping_) {
                return;
            }
        }

        rsp::proto::RSPMessage message;
        if (messageClient_ != nullptr && messageClient_->waitAndDequeueMessage(message)) {
            recordClientDequeueEvent(message, keyPair().nodeID());
            dispatchIncomingMessage(std::move(message));
        }
    }
}

void RSPClient::dispatchIncomingMessage(rsp::proto::RSPMessage message) {
    if (!shouldHandleLocally(message)) {
        return;
    }

    recordNodeInputEnqueueEvent(message, keyPair().nodeID());
    if (!enqueueInput(std::move(message))) {
        std::cerr << "RSPClient failed to enqueue inbound message on node input queue" << std::endl;
    }
}

bool RSPClient::shouldHandleLocally(const rsp::proto::RSPMessage& message) const {
    if (!message.has_destination()) {
        return true;
    }

    return message.destination().value() == toProtoNodeId(keyPair().nodeID()).value();
}

void RSPClient::handlePingReply(const rsp::proto::RSPMessage& message) {
    if (!message.ping_reply().has_nonce()) {
        return;
    }

    if (rsp::ping_trace::isEnabled()) {
        rsp::ping_trace::recordForMessage(message, "source_ping_reply_handler_start");
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto iterator = pendingPings_.find(message.ping_reply().nonce().value());
    if (iterator == pendingPings_.end()) {
        return;
    }

    if (message.ping_reply().sequence() != iterator->second.sequence) {
        return;
    }

    const auto sourceNodeId = rsp::senderNodeIdFromMessage(message);
    if (!sourceNodeId.has_value() || *sourceNodeId != iterator->second.destination) {
        return;
    }

    iterator->second.completed = true;
    if (rsp::ping_trace::isEnabled()) {
        rsp::ping_trace::recordForMessage(message, "source_ping_reply_completed");
    }
    stateChanged_.notify_all();
}

void RSPClient::handleEndorsementDone(const rsp::proto::RSPMessage& message) {
    const auto sourceNodeId = rsp::senderNodeIdFromMessage(message);
    if (!sourceNodeId.has_value()) {
        return;
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto iterator = pendingEndorsements_.find(toProtoNodeId(*sourceNodeId).value());
    if (iterator == pendingEndorsements_.end() || iterator->second.completed) {
        return;
    }

    iterator->second.completed = true;
    iterator->second.reply = message.endorsement_done();
    stateChanged_.notify_all();
}

void RSPClient::handleSocketReply(const rsp::proto::RSPMessage& message) {
    std::shared_ptr<NativeSocketBridgeState> nativeSocketBridge;
    const rsp::proto::SocketReply socketReply = message.socket_reply();

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto replySocketId = socketReply.has_socket_id()
                                       ? fromProtoSocketId(socketReply.socket_id())
                                       : std::optional<rsp::GUID>();
        if (replySocketId.has_value()) {
            const auto pendingIterator = pendingConnects_.find(*replySocketId);
            if (pendingIterator != pendingConnects_.end()) {
                const auto status = socketReply.error();
                if (!pendingIterator->second.completed &&
                    (status == rsp::proto::SUCCESS ||
                     status == rsp::proto::CONNECT_REFUSED ||
                     status == rsp::proto::CONNECT_TIMEOUT ||
                     status == rsp::proto::SOCKET_ERROR ||
                     status == rsp::proto::SOCKET_IN_USE ||
                     status == rsp::proto::INVALID_FLAGS)) {
                    pendingIterator->second.completed = true;
                    pendingIterator->second.reply = socketReply;
                    stateChanged_.notify_all();
                    return;
                }
            }

            const auto pendingListenIterator = pendingListens_.find(*replySocketId);
            if (pendingListenIterator != pendingListens_.end()) {
                const auto status = socketReply.error();
                if (!pendingListenIterator->second.completed &&
                    (status == rsp::proto::SUCCESS ||
                     status == rsp::proto::SOCKET_ERROR ||
                     status == rsp::proto::SOCKET_IN_USE ||
                     status == rsp::proto::INVALID_FLAGS)) {
                    pendingListenIterator->second.completed = true;
                    pendingListenIterator->second.reply = socketReply;
                    stateChanged_.notify_all();
                    return;
                }
            }

            if (socketReply.has_new_socket_id()) {
                const auto newSocketId = fromProtoSocketId(socketReply.new_socket_id());
                const auto sourceNodeId = rsp::senderNodeIdFromMessage(message);
                if (newSocketId.has_value() && sourceNodeId.has_value()) {
                    socketRoutes_[*newSocketId] = *sourceNodeId;
                }
            }

            const auto bridgeIterator = nativeSocketBridges_.find(*replySocketId);
            const auto awaitedIterator = awaitedSocketReplies_.find(*replySocketId);
            if (bridgeIterator != nativeSocketBridges_.end() && awaitedIterator == awaitedSocketReplies_.end() &&
                (socketReply.error() == rsp::proto::SOCKET_DATA ||
                 socketReply.error() == rsp::proto::SOCKET_CLOSED ||
                 socketReply.error() == rsp::proto::SOCKET_ERROR)) {
                nativeSocketBridge = bridgeIterator->second;
                if (socketReply.error() != rsp::proto::SOCKET_DATA) {
                    nativeSocketBridge->remoteClosed.store(true);
                    nativeSocketBridge->stopping.store(true);
                }
            } else {
                socketReplyQueues_[*replySocketId].push_back(socketReply);
                if (awaitedIterator != awaitedSocketReplies_.end()) {
                    awaitedSocketReplies_.erase(awaitedIterator);
                    stateChanged_.notify_all();
                    return;
                }
            }
        }

        if (nativeSocketBridge == nullptr) {
            pendingSocketReplies_.push_back(socketReply);
        }
        stateChanged_.notify_all();
    }

    if (nativeSocketBridge == nullptr) {
        return;
    }

    if (socketReply.error() == rsp::proto::SOCKET_DATA && socketReply.has_data()) {
        if (!sendAllToSocket(nativeSocketBridge->bridgeSocket,
                             reinterpret_cast<const uint8_t*>(socketReply.data().data()),
                             socketReply.data().size())) {
            nativeSocketBridge->stopping.store(true);
            rsp::os::closeSocket(nativeSocketBridge->bridgeSocket);
        }
        return;
    }

    rsp::os::closeSocket(nativeSocketBridge->bridgeSocket);
}

void RSPClient::handleResourceAdvertisement(const rsp::proto::RSPMessage& message) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    pendingResourceAdvertisements_.push_back(message.resource_advertisement());
    stateChanged_.notify_all();
}

void RSPClient::runNativeSocketBridge(const rsp::GUID& socketId,
                                      const std::shared_ptr<NativeSocketBridgeState>& bridgeState) {
    std::vector<uint8_t> buffer(4096);
    while (!bridgeState->stopping.load()) {
        const int bytesRead = rsp::os::recvSocket(bridgeState->bridgeSocket,
                                                  buffer.data(),
                                                  static_cast<uint32_t>(buffer.size()));
        if (bytesRead <= 0) {
            break;
        }

        const std::string payload(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(bytesRead));
        if (!socketSend(socketId, payload)) {
            break;
        }
    }

    bridgeState->stopping.store(true);
    rsp::os::closeSocket(bridgeState->bridgeSocket);
    if (!bridgeState->remoteClosed.load()) {
        socketClose(socketId);
    }

}

void RSPClient::runNativeListenSocketBridge(const std::shared_ptr<NativeListenBridgeState>& bridgeState) {
    while (!bridgeState->stopping.load()) {
        const auto reply = acceptTCPEx(bridgeState->listenSocketId, std::nullopt, 100, false, false, true);
        if (bridgeState->stopping.load()) {
            break;
        }

        if (!reply.has_value()) {
            break;
        }

        if (reply->error() == rsp::proto::TIMED_OUT) {
            continue;
        }

        if ((reply->error() != rsp::proto::SUCCESS && reply->error() != rsp::proto::NEW_CONNECTION) ||
            !reply->has_new_socket_id()) {
            if (reply->error() == rsp::proto::SOCKET_CLOSED) {
                break;
            }

            continue;
        }

        const auto socketId = fromProtoSocketId(reply->new_socket_id());
        if (!socketId.has_value()) {
            continue;
        }

        const auto bridgeSocket = rsp::os::connectLocalListenerSocket(bridgeState->localEndpoint);
        if (!rsp::os::isValidSocket(bridgeSocket)) {
            socketClose(*socketId);
            break;
        }

        const auto socketBridge = attachNativeSocketBridge(*socketId, bridgeSocket);
        startNativeSocketBridgeWorker(*socketId, socketBridge);
    }

    socketClose(bridgeState->listenSocketId);
}

void RSPClient::stopNativeSocketBridges() {
    std::vector<std::shared_ptr<NativeSocketBridgeState>> nativeSocketBridges;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (auto& [_, bridgeState] : nativeSocketBridges_) {
            nativeSocketBridges.push_back(bridgeState);
        }
        nativeSocketBridges_.clear();
    }

    for (auto& bridgeState : nativeSocketBridges) {
        bridgeState->remoteClosed.store(true);
        bridgeState->stopping.store(true);
        rsp::os::closeSocket(bridgeState->bridgeSocket);
        if (bridgeState->worker.joinable() && bridgeState->worker.get_id() != std::this_thread::get_id()) {
            bridgeState->worker.join();
        }
    }
}

void RSPClient::stopNativeListenBridges() {
    std::vector<std::shared_ptr<NativeListenBridgeState>> nativeListenBridges;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (auto& [_, bridgeState] : nativeListenBridges_) {
            nativeListenBridges.push_back(bridgeState);
        }
        nativeListenBridges_.clear();
    }

    for (auto& bridgeState : nativeListenBridges) {
        bridgeState->stopping.store(true);
    }

    for (auto& bridgeState : nativeListenBridges) {
        if (bridgeState->worker.joinable() && bridgeState->worker.get_id() != std::this_thread::get_id()) {
            bridgeState->worker.join();
        }
    }
}

void RSPClient::stopNativeSocketBridgesForNode(const rsp::NodeID& nodeId) {
    std::map<rsp::GUID, std::shared_ptr<NativeSocketBridgeState>> nativeSocketBridges;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (auto iterator = nativeSocketBridges_.begin(); iterator != nativeSocketBridges_.end();) {
            const auto routeIterator = socketRoutes_.find(iterator->first);
            if (routeIterator != socketRoutes_.end() && routeIterator->second == nodeId) {
                nativeSocketBridges.emplace(iterator->first, iterator->second);
                iterator = nativeSocketBridges_.erase(iterator);
                continue;
            }

            ++iterator;
        }
    }

    for (auto& [_, bridgeState] : nativeSocketBridges) {
        bridgeState->remoteClosed.store(true);
        bridgeState->stopping.store(true);
        rsp::os::closeSocket(bridgeState->bridgeSocket);
        if (bridgeState->worker.joinable() && bridgeState->worker.get_id() != std::this_thread::get_id()) {
            bridgeState->worker.join();
        }
    }
}

void RSPClient::stopNativeListenBridgesForNode(const rsp::NodeID& nodeId) {
    std::map<rsp::GUID, std::shared_ptr<NativeListenBridgeState>> nativeListenBridges;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (auto iterator = nativeListenBridges_.begin(); iterator != nativeListenBridges_.end();) {
            if (iterator->second->nodeId == nodeId) {
                nativeListenBridges.emplace(iterator->first, iterator->second);
                iterator = nativeListenBridges_.erase(iterator);
                continue;
            }

            ++iterator;
        }
    }

    for (auto& [_, bridgeState] : nativeListenBridges) {
        bridgeState->stopping.store(true);
    }

    for (auto& [_, bridgeState] : nativeListenBridges) {
        if (bridgeState->worker.joinable() && bridgeState->worker.get_id() != std::this_thread::get_id()) {
            bridgeState->worker.join();
        }
    }
}

rsp::proto::NodeId RSPClient::toProtoNodeId(const rsp::NodeID& nodeId) {
    return ::rsp::client::toProtoNodeId(nodeId);
}

rsp::proto::SocketID RSPClient::toProtoSocketId(const rsp::GUID& socketId) {
    rsp::proto::SocketID protoSocketId;
    std::string value(16, '\0');
    const uint64_t high = socketId.high();
    const uint64_t low = socketId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoSocketId.set_value(value);
    return protoSocketId;
}

std::optional<rsp::GUID> RSPClient::fromProtoSocketId(const rsp::proto::SocketID& socketId) {
    if (socketId.value().size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, socketId.value().data(), sizeof(high));
    std::memcpy(&low, socketId.value().data() + sizeof(high), sizeof(low));
    return rsp::GUID(high, low);
}

}  // namespace rsp::client