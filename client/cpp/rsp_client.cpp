#include "client/cpp/rsp_client.hpp"

#include "common/base_types.hpp"
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
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stopping_ = true;
    }
    stateChanged_.notify_all();

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
}

int RSPClient::run() const {
    return 0;
}

bool RSPClient::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_ping_reply()) {
        handlePingReply(message);
        return true;
    }

    if (message.has_socket_reply()) {
        handleSocketReply(message);
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
    return messageClient_->removeConnection(connectionId);
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
    *pingRequest.mutable_source() = toProtoNodeId(keyPair().nodeID());
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

std::optional<rsp::proto::SocketReply> RSPClient::connectTCPReply(rsp::NodeID nodeId,
                                                                  const std::string& hostPort,
                                                                  uint32_t timeoutMilliseconds,
                                                                  uint32_t retries,
                                                                  uint32_t retryMilliseconds,
                                                                  bool asyncData,
                                                                  bool shareSocket,
                                                                  bool useSocket) {
    const rsp::GUID socketId;
    rsp::proto::RSPMessage request;
    *request.mutable_source() = toProtoNodeId(keyPair().nodeID());
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
    const auto reply = connectTCPReply(nodeId,
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

bool RSPClient::socketSend(const rsp::GUID& socketId, const std::string& data) {
    rsp::NodeID destination;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto iterator = socketRoutes_.find(socketId);
        if (iterator == socketRoutes_.end()) {
            return true;
        }

        destination = iterator->second;
        awaitedSocketReplies_.insert(socketId);
    }

    rsp::proto::RSPMessage request;
    *request.mutable_source() = toProtoNodeId(keyPair().nodeID());
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
    const auto reply = socketRecvReply(socketId, maxBytes, waitMilliseconds);
    if (!reply.has_value()) {
        return std::nullopt;
    }

    if (reply->error() != rsp::proto::SOCKET_DATA && reply->error() != rsp::proto::SUCCESS) {
        return std::nullopt;
    }

    return reply->has_data() ? std::optional<std::string>(reply->data()) : std::optional<std::string>(std::string());
}

std::optional<rsp::proto::SocketReply> RSPClient::socketRecvReply(const rsp::GUID& socketId,
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
    *request.mutable_source() = toProtoNodeId(keyPair().nodeID());
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
    *request.mutable_source() = toProtoNodeId(keyPair().nodeID());
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

        if (reply->error() != rsp::proto::SOCKET_DATA && reply->error() != rsp::proto::ASYNC_SOCKET) {
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
        if (messageClient_ != nullptr && messageClient_->tryDequeueMessage(message)) {
            recordClientDequeueEvent(message, keyPair().nodeID());
            dispatchIncomingMessage(std::move(message));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

    if (message.source().value() != toProtoNodeId(iterator->second.destination).value()) {
        return;
    }

    iterator->second.completed = true;
    if (rsp::ping_trace::isEnabled()) {
        rsp::ping_trace::recordForMessage(message, "source_ping_reply_completed");
    }
    stateChanged_.notify_all();
}

void RSPClient::handleSocketReply(const rsp::proto::RSPMessage& message) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto replySocketId = message.socket_reply().has_socket_id()
                                   ? fromProtoSocketId(message.socket_reply().socket_id())
                                   : std::optional<rsp::GUID>();
    if (replySocketId.has_value()) {
        const auto pendingIterator = pendingConnects_.find(*replySocketId);
        if (pendingIterator != pendingConnects_.end()) {
            const auto status = message.socket_reply().error();
            if (!pendingIterator->second.completed &&
                (status == rsp::proto::SUCCESS ||
                 status == rsp::proto::CONNECT_REFUSED ||
                 status == rsp::proto::CONNECT_TIMEOUT ||
                 status == rsp::proto::SOCKET_ERROR ||
                 status == rsp::proto::INVALID_FLAGS)) {
                pendingIterator->second.completed = true;
                pendingIterator->second.reply = message.socket_reply();
                stateChanged_.notify_all();
                return;
            }
        }

        socketReplyQueues_[*replySocketId].push_back(message.socket_reply());
        const auto awaitedIterator = awaitedSocketReplies_.find(*replySocketId);
        if (awaitedIterator != awaitedSocketReplies_.end()) {
            awaitedSocketReplies_.erase(awaitedIterator);
            stateChanged_.notify_all();
            return;
        }
    }

    pendingSocketReplies_.push_back(message.socket_reply());
    stateChanged_.notify_all();
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