#include "client/cpp/rsp_client.hpp"

#include "common/base_types.hpp"
#include "common/endorsement/endorsement.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "common/ping_trace.hpp"
#include "common/service_message.hpp"

#include "resource_service/bsd_sockets/bsd_sockets.pb.h"
#include "resource_service/sshd/sshd.pb.h"
#include "resource_service/httpd/httpd.pb.h"
#include "name_service/name_service.pb.h"

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

rsp::proto::Uuid toProtoUuid(const rsp::GUID& guid) {
    rsp::proto::Uuid protoUuid;
    std::string value(16, '\0');
    const uint64_t high = guid.high();
    const uint64_t low = guid.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoUuid.set_value(value);
    return protoUuid;
}

bool isNameServiceReply(const rsp::proto::RSPMessage& message) {
    return rsp::hasServiceMessage<rsp::proto::NameCreateReply>(message) ||
           rsp::hasServiceMessage<rsp::proto::NameReadReply>(message) ||
           rsp::hasServiceMessage<rsp::proto::NameUpdateReply>(message) ||
           rsp::hasServiceMessage<rsp::proto::NameDeleteReply>(message) ||
           rsp::hasServiceMessage<rsp::proto::NameQueryReply>(message);
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

bool streamRepliesMatch(const rsp::proto::StreamReply& left, const rsp::proto::StreamReply& right) {
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

void removeBufferedReply(std::deque<rsp::proto::StreamReply>& pendingReplies,
                         const rsp::proto::StreamReply& reply) {
    for (auto iterator = pendingReplies.begin(); iterator != pendingReplies.end(); ++iterator) {
        if (streamRepliesMatch(*iterator, reply)) {
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

    if (rsp::hasServiceMessage<rsp::proto::EndorsementDone>(message)) {
        handleEndorsementDone(message);
        return true;
    }

    if (rsp::hasServiceMessage<rsp::proto::StreamReply>(message)) {
        handleStreamReply(message);
        return true;
    }

    if (message.has_resource_advertisement()) {
        handleResourceAdvertisement(message);
        return true;
    }

    if (message.has_resource_query_reply()) {
        handleResourceQueryReply(message);
        return true;
    }

    if (message.has_schema_reply()) {
        handleSchemaReply(message);
        return true;
    }

    if (isNameServiceReply(message)) {
        handleNameServiceReply(message);
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
    *identityMessage.add_identities()->mutable_public_key() = keyPair().publicKey();
    return messageClient_ != nullptr && messageClient_->send(identityMessage);
}

bool RSPClient::sendBeginEndorsementRequestMessage(rsp::NodeID nodeId,
                                                   const rsp::proto::Endorsement& requestedMessage) {
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::proto::BeginEndorsementRequest beginReq;
    *beginReq.mutable_requested_values() = requestedMessage;
    rsp::packServiceMessage(request, beginReq);
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

std::optional<rsp::proto::ResourceQueryReply> RSPClient::resourceList(
        rsp::NodeID nodeId, const std::string& query, uint32_t maxRecords) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        resourceListPending_ = true;
        resourceListResult_.reset();
    }

    if (!queryResources(nodeId, query, maxRecords)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        resourceListPending_ = false;
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool replied = stateChanged_.wait_for(lock, std::chrono::seconds(5), [this]() {
        return stopping_ || !resourceListPending_;
    });

    if (!replied || stopping_ || !resourceListResult_.has_value()) {
        resourceListPending_ = false;
        return std::nullopt;
    }

    auto result = std::move(resourceListResult_);
    resourceListResult_.reset();
    return result;
}

// ---------------------------------------------------------------------------
// Name service wrappers
// ---------------------------------------------------------------------------

template <typename ReplyT, typename RequestT>
std::optional<ReplyT> sendNameRequest(RSPClient& self,
                                      rsp::NodeID nodeId,
                                      const RequestT& serviceReq,
                                      std::mutex& stateMutex,
                                      std::condition_variable& stateChanged,
                                      bool& nameReplyPending,
                                      std::optional<rsp::proto::RSPMessage>& nameReplyMessage,
                                      const bool& stopping,
                                      RSPClientMessage::Ptr& messageClient) {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        nameReplyPending = true;
        nameReplyMessage.reset();
    }

    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::packServiceMessage(request, serviceReq);

    if (!messageClient->send(request)) {
        std::lock_guard<std::mutex> lock(stateMutex);
        nameReplyPending = false;
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lock(stateMutex);
    const bool replied = stateChanged.wait_for(lock, std::chrono::seconds(5), [&]() {
        return stopping || !nameReplyPending;
    });

    if (!replied || stopping || !nameReplyMessage.has_value()) {
        nameReplyPending = false;
        return std::nullopt;
    }

    ReplyT reply;
    if (!rsp::unpackServiceMessage(*nameReplyMessage, &reply)) {
        nameReplyMessage.reset();
        return std::nullopt;
    }
    nameReplyMessage.reset();
    return reply;
}

std::optional<rsp::proto::NameCreateReply> RSPClient::nameCreate(
        rsp::NodeID nodeId, const std::string& name,
        rsp::NodeID owner, const rsp::GUID& type, const rsp::GUID& value) {
    rsp::proto::NameCreateRequest req;
    auto* record = req.mutable_record();
    record->set_name(name);
    *record->mutable_owner() = toProtoNodeId(owner);
    *record->mutable_type() = toProtoUuid(type);
    *record->mutable_value() = toProtoUuid(value);
    return sendNameRequest<rsp::proto::NameCreateReply>(*this, nodeId, req,
        stateMutex_, stateChanged_, nameReplyPending_, nameReplyMessage_, stopping_, messageClient_);
}

std::optional<rsp::proto::NameReadReply> RSPClient::nameRead(
        rsp::NodeID nodeId, const std::string& name,
        const std::optional<rsp::NodeID>& owner, const std::optional<rsp::GUID>& type) {
    rsp::proto::NameReadRequest req;
    req.set_name(name);
    if (owner.has_value()) {
        *req.mutable_owner() = toProtoNodeId(*owner);
    }
    if (type.has_value()) {
        *req.mutable_type() = toProtoUuid(*type);
    }
    return sendNameRequest<rsp::proto::NameReadReply>(*this, nodeId, req,
        stateMutex_, stateChanged_, nameReplyPending_, nameReplyMessage_, stopping_, messageClient_);
}

std::optional<rsp::proto::NameUpdateReply> RSPClient::nameUpdate(
        rsp::NodeID nodeId, const std::string& name,
        rsp::NodeID owner, const rsp::GUID& type, const rsp::GUID& newValue) {
    rsp::proto::NameUpdateRequest req;
    req.set_name(name);
    *req.mutable_owner() = toProtoNodeId(owner);
    *req.mutable_type() = toProtoUuid(type);
    *req.mutable_new_value() = toProtoUuid(newValue);
    return sendNameRequest<rsp::proto::NameUpdateReply>(*this, nodeId, req,
        stateMutex_, stateChanged_, nameReplyPending_, nameReplyMessage_, stopping_, messageClient_);
}

std::optional<rsp::proto::NameDeleteReply> RSPClient::nameDelete(
        rsp::NodeID nodeId, const std::string& name,
        rsp::NodeID owner, const rsp::GUID& type) {
    rsp::proto::NameDeleteRequest req;
    req.set_name(name);
    *req.mutable_owner() = toProtoNodeId(owner);
    *req.mutable_type() = toProtoUuid(type);
    return sendNameRequest<rsp::proto::NameDeleteReply>(*this, nodeId, req,
        stateMutex_, stateChanged_, nameReplyPending_, nameReplyMessage_, stopping_, messageClient_);
}

std::optional<rsp::proto::NameQueryReply> RSPClient::nameQuery(
        rsp::NodeID nodeId, const std::string& namePrefix,
        const std::optional<rsp::NodeID>& owner, const std::optional<rsp::GUID>& type,
        uint32_t maxRecords) {
    rsp::proto::NameQueryRequest req;
    if (!namePrefix.empty()) {
        req.set_name_prefix(namePrefix);
    }
    if (owner.has_value()) {
        *req.mutable_owner() = toProtoNodeId(*owner);
    }
    if (type.has_value()) {
        *req.mutable_type() = toProtoUuid(*type);
    }
    if (maxRecords > 0) {
        req.set_max_records(maxRecords);
    }
    return sendNameRequest<rsp::proto::NameQueryReply>(*this, nodeId, req,
        stateMutex_, stateChanged_, nameReplyPending_, nameReplyMessage_, stopping_, messageClient_);
}

std::optional<rsp::proto::StreamReply> RSPClient::connectTCPEx(rsp::NodeID nodeId,
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
    rsp::proto::ConnectTCPRequest connectReq;
    connectReq.set_host_port(hostPort);
    *connectReq.mutable_stream_id() = toProtoStreamId(socketId);
    connectReq.set_use_socket(useSocket);
    if (timeoutMilliseconds > 0) {
        connectReq.set_timeout_ms(timeoutMilliseconds);
    }
    if (retries > 0) {
        connectReq.set_retries(retries);
    }
    if (retryMilliseconds > 0) {
        connectReq.set_retry_ms(retryMilliseconds);
    }
    if (asyncData) {
        connectReq.set_async_data(true);
    }
    if (shareSocket) {
        connectReq.set_share_socket(true);
    }
    rsp::packServiceMessage(request, connectReq);

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
        if (!reply->has_stream_id()) {
            *reply->mutable_stream_id() = toProtoStreamId(socketId);
        }
        streamRoutes_[socketId] = nodeId;
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
    if (!reply.has_value() || reply->error() != rsp::proto::SUCCESS || !reply->has_stream_id()) {
        return std::nullopt;
    }

    return fromProtoStreamId(reply->stream_id());
}

std::optional<rsp::proto::StreamReply> RSPClient::listenTCPEx(rsp::NodeID nodeId,
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
    rsp::proto::ListenTCPRequest listenReq;
    listenReq.set_host_port(hostPort);
    *listenReq.mutable_stream_id() = toProtoStreamId(socketId);
    if (timeoutMilliseconds > 0) {
        listenReq.set_timeout_ms(timeoutMilliseconds);
    }
    if (asyncAccept) {
        listenReq.set_async_accept(true);
    }
    if (shareListeningSocket) {
        listenReq.set_share_listening_socket(true);
    }
    if (shareChildSockets) {
        listenReq.set_share_child_sockets(true);
    }
    if (childrenUseSocket) {
        listenReq.set_children_use_socket(true);
    }
    if (childrenAsyncData) {
        listenReq.set_children_async_data(true);
    }
    rsp::packServiceMessage(request, listenReq);

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
        if (!reply->has_stream_id()) {
            *reply->mutable_stream_id() = toProtoStreamId(socketId);
        }
        streamRoutes_[socketId] = nodeId;
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
    if (!reply.has_value() || reply->error() != rsp::proto::SUCCESS || !reply->has_stream_id()) {
        return std::nullopt;
    }

    return fromProtoStreamId(reply->stream_id());
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
        streamClose(*listenSocketId);
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

std::optional<rsp::proto::StreamReply> RSPClient::acceptTCPEx(const rsp::GUID& listenSocketId,
                                                              const std::optional<rsp::GUID>& newSocketId,
                                                              uint32_t timeoutMilliseconds,
                                                              bool shareChildSocket,
                                                              bool childUseSocket,
                                                              bool childAsyncData) {
    rsp::NodeID destination;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto iterator = streamRoutes_.find(listenSocketId);
        if (iterator == streamRoutes_.end()) {
            return std::nullopt;
        }

        destination = iterator->second;
        awaitedStreamReplies_.insert(listenSocketId);
    }

    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(destination);
    rsp::proto::AcceptTCP acceptReq;
    *acceptReq.mutable_listen_stream_id() = toProtoStreamId(listenSocketId);
    if (newSocketId.has_value()) {
        *acceptReq.mutable_new_stream_id() = toProtoStreamId(*newSocketId);
    }
    if (timeoutMilliseconds > 0) {
        acceptReq.set_timeout_ms(timeoutMilliseconds);
    }
    if (shareChildSocket) {
        acceptReq.set_share_child_socket(true);
    }
    if (childUseSocket) {
        acceptReq.set_child_use_socket(true);
    }
    if (childAsyncData) {
        acceptReq.set_child_async_data(true);
    }
    rsp::packServiceMessage(request, acceptReq);

    if (!messageClient_->send(request)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        awaitedStreamReplies_.erase(listenSocketId);
        return std::nullopt;
    }

    auto reply = waitForStreamReply(listenSocketId);
    if (reply.has_value() &&
        (reply->error() == rsp::proto::SUCCESS || reply->error() == rsp::proto::NEW_CONNECTION) &&
        reply->has_new_stream_id()) {
        const auto acceptedSocketId = fromProtoStreamId(reply->new_stream_id());
        if (acceptedSocketId.has_value()) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            streamRoutes_[*acceptedSocketId] = destination;
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
        !reply->has_new_stream_id()) {
        return std::nullopt;
    }

    return fromProtoStreamId(reply->new_stream_id());
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

    const auto bridgeState = attachNativeStreamBridge(*socketId, bridgeSocket);
    startNativeStreamBridgeWorker(*socketId, bridgeState);

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

    const auto bridgeState = attachNativeStreamBridge(*socketId, bridgeSocket);
    startNativeStreamBridgeWorker(*socketId, bridgeState);

    return applicationSocket;
}

std::optional<rsp::proto::StreamReply> RSPClient::connectSshdEx(rsp::NodeID nodeId,
                                                                  uint32_t timeoutMilliseconds,
                                                                  bool asyncData,
                                                                  bool shareSocket,
                                                                  bool useSocket) {
    const rsp::GUID socketId;
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::proto::ConnectSshd connectReq;
    *connectReq.mutable_stream_id() = toProtoStreamId(socketId);
    connectReq.set_use_socket(useSocket);
    if (timeoutMilliseconds > 0) {
        connectReq.set_timeout_ms(timeoutMilliseconds);
    }
    if (asyncData) {
        connectReq.set_async_data(true);
    }
    if (shareSocket) {
        connectReq.set_share_socket(true);
    }
    rsp::packServiceMessage(request, connectReq);

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
        if (!reply->has_stream_id()) {
            *reply->mutable_stream_id() = toProtoStreamId(socketId);
        }
        streamRoutes_[socketId] = nodeId;
    }

    return reply;
}

std::optional<rsp::GUID> RSPClient::connectSshd(rsp::NodeID nodeId,
                                                  uint32_t timeoutMilliseconds,
                                                  bool asyncData,
                                                  bool shareSocket,
                                                  bool useSocket) {
    const auto reply = connectSshdEx(nodeId, timeoutMilliseconds, asyncData, shareSocket, useSocket);
    if (!reply.has_value() || reply->error() != rsp::proto::SUCCESS || !reply->has_stream_id()) {
        return std::nullopt;
    }
    return fromProtoStreamId(reply->stream_id());
}

std::optional<rsp::os::SocketHandle> RSPClient::connectSshdSocket(rsp::NodeID nodeId,
                                                                    uint32_t timeoutMilliseconds) {
    rsp::os::SocketHandle applicationSocket = rsp::os::invalidSocket();
    rsp::os::SocketHandle bridgeSocket = rsp::os::invalidSocket();
    if (!rsp::os::createSocketPair(applicationSocket, bridgeSocket)) {
        return std::nullopt;
    }

    const auto socketId = connectSshd(nodeId, timeoutMilliseconds, true, false, false);
    if (!socketId.has_value()) {
        rsp::os::closeSocket(applicationSocket);
        rsp::os::closeSocket(bridgeSocket);
        return std::nullopt;
    }

    const auto bridgeState = attachNativeStreamBridge(*socketId, bridgeSocket);
    startNativeStreamBridgeWorker(*socketId, bridgeState);

    return applicationSocket;
}


std::optional<rsp::proto::StreamReply> RSPClient::connectHttpEx(rsp::NodeID nodeId,
                                                                  uint32_t timeoutMilliseconds,
                                                                  bool asyncData,
                                                                  bool shareSocket) {
    const rsp::GUID socketId;
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::proto::ConnectHttp connectReq;
    *connectReq.mutable_stream_id() = toProtoStreamId(socketId);
    if (timeoutMilliseconds > 0) {
        connectReq.set_timeout_ms(timeoutMilliseconds);
    }
    if (asyncData) {
        connectReq.set_async_data(true);
    }
    if (shareSocket) {
        connectReq.set_share_socket(true);
    }
    rsp::packServiceMessage(request, connectReq);

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
        const auto it = pendingConnects_.find(socketId);
        return stopping_ || (it != pendingConnects_.end() && it->second.completed);
    });
    const auto it = pendingConnects_.find(socketId);
    if (!replied || stopping_ || it == pendingConnects_.end()) {
        pendingConnects_.erase(socketId);
        return std::nullopt;
    }

    auto reply = it->second.reply;
    pendingConnects_.erase(it);
    if (reply.has_value() && reply->error() == rsp::proto::SUCCESS) {
        if (!reply->has_stream_id()) {
            *reply->mutable_stream_id() = toProtoStreamId(socketId);
        }
        streamRoutes_[socketId] = nodeId;
    }

    return reply;
}

std::optional<rsp::GUID> RSPClient::connectHttp(rsp::NodeID nodeId,
                                                  uint32_t timeoutMilliseconds,
                                                  bool asyncData,
                                                  bool shareSocket) {
    const auto reply = connectHttpEx(nodeId, timeoutMilliseconds, asyncData, shareSocket);
    if (!reply.has_value() || reply->error() != rsp::proto::SUCCESS || !reply->has_stream_id()) {
        return std::nullopt;
    }

    uint64_t high = 0, low = 0;
    std::memcpy(&high, reply->stream_id().value().data(), sizeof(high));
    std::memcpy(&low,  reply->stream_id().value().data() + sizeof(high), sizeof(low));
    return rsp::GUID(high, low);
}

std::shared_ptr<RSPClient::NativeStreamBridgeState> RSPClient::attachNativeStreamBridge(
    const rsp::GUID& socketId,
    rsp::os::SocketHandle bridgeSocket) {
    auto bridgeState = std::make_shared<NativeStreamBridgeState>();
    bridgeState->bridgeSocket = bridgeSocket;

    std::deque<rsp::proto::StreamReply> bufferedReplies;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        nativeStreamBridges_[socketId] = bridgeState;
        const auto queuedReplies = streamReplyQueues_.find(socketId);
        if (queuedReplies != streamReplyQueues_.end()) {
            bufferedReplies = std::move(queuedReplies->second);
            streamReplyQueues_.erase(queuedReplies);
            for (const auto& reply : bufferedReplies) {
                removeBufferedReply(pendingStreamReplies_, reply);
            }
        }
    }

    for (const auto& reply : bufferedReplies) {
        if (reply.error() == rsp::proto::STREAM_DATA && reply.has_data()) {
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

void RSPClient::startNativeStreamBridgeWorker(const rsp::GUID& socketId,
                                              const std::shared_ptr<NativeStreamBridgeState>& bridgeState) {
    bridgeState->worker = std::thread([weakSelf = weak_from_this(), socketId, bridgeState]() {
        if (const auto self = weakSelf.lock()) {
            self->runNativeStreamBridge(socketId, bridgeState);
        } else {
            rsp::os::closeSocket(bridgeState->bridgeSocket);
        }
    });
}

bool RSPClient::streamSend(const rsp::GUID& socketId, const std::string& data) {
    rsp::NodeID destination;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto iterator = streamRoutes_.find(socketId);
        if (iterator == streamRoutes_.end()) {
            return false;
        }

        destination = iterator->second;
        awaitedStreamReplies_.insert(socketId);
    }

    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(destination);
    rsp::proto::StreamSend sendReq;
    *sendReq.mutable_stream_id() = toProtoStreamId(socketId);
    sendReq.set_data(data);
    rsp::packServiceMessage(request, sendReq);

    if (!messageClient_->send(request)) {
        return false;
    }

    const auto reply = waitForStreamReply(socketId);
    if (!reply.has_value()) {
        return false;
    }

    return reply->error() == rsp::proto::SUCCESS;
}

std::optional<std::string> RSPClient::streamRecv(const rsp::GUID& socketId,
                                                 uint32_t maxBytes,
                                                 uint32_t waitMilliseconds) {
    const auto reply = streamRecvEx(socketId, maxBytes, waitMilliseconds);
    if (!reply.has_value()) {
        return std::nullopt;
    }

    if (reply->error() != rsp::proto::STREAM_DATA && reply->error() != rsp::proto::SUCCESS) {
        return std::nullopt;
    }

    return reply->has_data() ? std::optional<std::string>(reply->data()) : std::optional<std::string>(std::string());
}

std::optional<rsp::proto::StreamReply> RSPClient::streamRecvEx(const rsp::GUID& socketId,
                                                                  uint32_t maxBytes,
                                                                  uint32_t waitMilliseconds) {
    rsp::NodeID destination;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto iterator = streamRoutes_.find(socketId);
        if (iterator == streamRoutes_.end()) {
            return std::nullopt;
        }

        destination = iterator->second;
        awaitedStreamReplies_.insert(socketId);
    }

    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(destination);
    rsp::proto::StreamRecv recvReq;
    *recvReq.mutable_stream_id() = toProtoStreamId(socketId);
    recvReq.set_max_bytes(maxBytes);
    if (waitMilliseconds > 0) {
        recvReq.set_wait_ms(waitMilliseconds);
    }
    rsp::packServiceMessage(request, recvReq);

    if (!messageClient_->send(request)) {
        return std::nullopt;
    }

    return waitForStreamReply(socketId);
}

std::optional<rsp::proto::StreamReply> RSPClient::waitForStreamReply(const rsp::GUID& socketId) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool replied = stateChanged_.wait_for(lock, std::chrono::seconds(5), [this, &socketId]() {
        const auto iterator = streamReplyQueues_.find(socketId);
        return stopping_ || (iterator != streamReplyQueues_.end() && !iterator->second.empty());
    });
    const auto iterator = streamReplyQueues_.find(socketId);
    if (!replied || stopping_ || iterator == streamReplyQueues_.end() || iterator->second.empty()) {
        awaitedStreamReplies_.erase(socketId);
        return std::nullopt;
    }

    const rsp::proto::StreamReply reply = iterator->second.front();
    iterator->second.pop_front();
    awaitedStreamReplies_.erase(socketId);
    for (auto globalIterator = pendingStreamReplies_.begin(); globalIterator != pendingStreamReplies_.end(); ++globalIterator) {
        if (streamRepliesMatch(*globalIterator, reply)) {
            pendingStreamReplies_.erase(globalIterator);
            break;
        }
    }
    if (iterator->second.empty()) {
        streamReplyQueues_.erase(iterator);
    }
    return reply;
}

bool RSPClient::streamClose(const rsp::GUID& socketId) {
    rsp::NodeID destination;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto iterator = streamRoutes_.find(socketId);
        if (iterator == streamRoutes_.end()) {
            return false;
        }

        destination = iterator->second;
        awaitedStreamReplies_.insert(socketId);
    }

    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(destination);
    rsp::proto::StreamClose closeReq;
    *closeReq.mutable_stream_id() = toProtoStreamId(socketId);
    rsp::packServiceMessage(request, closeReq);

    if (!messageClient_->send(request)) {
        streamRoutes_.erase(socketId);
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto reply = waitForStreamReply(socketId);
        if (!reply.has_value()) {
            streamRoutes_.erase(socketId);
            return true;
        }

        if (reply->error() == rsp::proto::SUCCESS || reply->error() == rsp::proto::STREAM_CLOSED) {
            streamRoutes_.erase(socketId);
            return true;
        }

        if (reply->error() != rsp::proto::STREAM_DATA &&
            reply->error() != rsp::proto::ASYNC_STREAM &&
            reply->error() != rsp::proto::NEW_CONNECTION) {
            return false;
        }
    }

    streamRoutes_.erase(socketId);
    return true;
}

bool RSPClient::tryDequeueStreamReply(rsp::proto::StreamReply& reply) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (pendingStreamReplies_.empty()) {
        return false;
    }

    reply = pendingStreamReplies_.front();
    pendingStreamReplies_.pop_front();
    return true;
}

std::size_t RSPClient::pendingStreamReplyCount() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return pendingStreamReplies_.size();
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

bool RSPClient::tryDequeueResourceQueryReply(rsp::proto::ResourceQueryReply& reply) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (pendingResourceQueryReplies_.empty()) {
        return false;
    }

    reply = pendingResourceQueryReplies_.front();
    pendingResourceQueryReplies_.pop_front();
    return true;
}

std::size_t RSPClient::pendingResourceQueryReplyCount() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return pendingResourceQueryReplies_.size();
}

bool RSPClient::querySchemas(rsp::NodeID nodeId,
                             const std::string& protoFileName,
                             const std::string& schemaHash) {
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    auto* schemaRequest = request.mutable_schema_request();
    if (!protoFileName.empty()) {
        schemaRequest->set_proto_file_name(protoFileName);
    }
    if (!schemaHash.empty()) {
        schemaRequest->set_schema_hash(schemaHash);
    }
    return messageClient_->send(request);
}

bool RSPClient::tryDequeueSchemaReply(rsp::proto::SchemaReply& reply) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (pendingSchemaReplies_.empty()) {
        return false;
    }

    reply = pendingSchemaReplies_.front();
    pendingSchemaReplies_.pop_front();
    return true;
}

std::size_t RSPClient::pendingSchemaReplyCount() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return pendingSchemaReplies_.size();
}

void RSPClient::handleSchemaReply(const rsp::proto::RSPMessage& message) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    pendingSchemaReplies_.push_back(message.schema_reply());
    stateChanged_.notify_all();
}

void RSPClient::handleResourceQueryReply(const rsp::proto::RSPMessage& message) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (resourceListPending_) {
        resourceListResult_ = message.resource_query_reply();
        resourceListPending_ = false;
    } else {
        pendingResourceQueryReplies_.push_back(message.resource_query_reply());
    }
    stateChanged_.notify_all();
}

void RSPClient::registerStreamRoute(const rsp::GUID& socketId, rsp::NodeID nodeId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    streamRoutes_[socketId] = nodeId;
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
    rsp::proto::EndorsementDone done;
    rsp::unpackServiceMessage(message, &done);
    iterator->second.reply = done;
    stateChanged_.notify_all();
}

void RSPClient::handleStreamReply(const rsp::proto::RSPMessage& message) {
    std::shared_ptr<NativeStreamBridgeState> nativeSocketBridge;
    rsp::proto::StreamReply socketReply;
    rsp::unpackServiceMessage(message, &socketReply);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto replySocketId = socketReply.has_stream_id()
                                       ? fromProtoStreamId(socketReply.stream_id())
                                       : std::optional<rsp::GUID>();
        if (replySocketId.has_value()) {
            const auto pendingIterator = pendingConnects_.find(*replySocketId);
            if (pendingIterator != pendingConnects_.end()) {
                const auto status = socketReply.error();
                if (!pendingIterator->second.completed &&
                    (status == rsp::proto::SUCCESS ||
                     status == rsp::proto::CONNECT_REFUSED ||
                     status == rsp::proto::CONNECT_TIMEOUT ||
                     status == rsp::proto::STREAM_ERROR ||
                     status == rsp::proto::STREAM_IN_USE ||
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
                     status == rsp::proto::STREAM_ERROR ||
                     status == rsp::proto::STREAM_IN_USE ||
                     status == rsp::proto::INVALID_FLAGS)) {
                    pendingListenIterator->second.completed = true;
                    pendingListenIterator->second.reply = socketReply;
                    stateChanged_.notify_all();
                    return;
                }
            }

            if (socketReply.has_new_stream_id()) {
                const auto newSocketId = fromProtoStreamId(socketReply.new_stream_id());
                const auto sourceNodeId = rsp::senderNodeIdFromMessage(message);
                if (newSocketId.has_value() && sourceNodeId.has_value()) {
                    streamRoutes_[*newSocketId] = *sourceNodeId;
                }
            }

            const auto bridgeIterator = nativeStreamBridges_.find(*replySocketId);
            const auto awaitedIterator = awaitedStreamReplies_.find(*replySocketId);
            const bool isSocketData = socketReply.error() == rsp::proto::STREAM_DATA;
            const bool isDataOrClose = isSocketData ||
                                       socketReply.error() == rsp::proto::STREAM_CLOSED ||
                                       socketReply.error() == rsp::proto::STREAM_ERROR;
            // STREAM_DATA must always reach the bridge immediately even while we are
            // awaiting a SUCCESS reply to a SOCKET_SEND; otherwise the bridge worker
            // blocks in streamSend() and never drains the incoming data, causing a
            // deadlock during full-duplex streams (e.g. SSH handshake).
            // STREAM_CLOSED/STREAM_ERROR also go to the bridge, but only when there is
            // no pending waiter; if a waiter exists we let the error wake it instead.
            if (bridgeIterator != nativeStreamBridges_.end() && isDataOrClose &&
                (isSocketData || awaitedIterator == awaitedStreamReplies_.end())) {
                nativeSocketBridge = bridgeIterator->second;
                if (!isSocketData) {
                    nativeSocketBridge->remoteClosed.store(true);
                    nativeSocketBridge->stopping.store(true);
                }
            } else {
                streamReplyQueues_[*replySocketId].push_back(socketReply);
                if (awaitedIterator != awaitedStreamReplies_.end()) {
                    awaitedStreamReplies_.erase(awaitedIterator);
                    stateChanged_.notify_all();
                    return;
                }
            }
        }

        if (nativeSocketBridge == nullptr) {
            pendingStreamReplies_.push_back(socketReply);
        }
        stateChanged_.notify_all();
    }

    if (nativeSocketBridge == nullptr) {
        return;
    }

    if (socketReply.error() == rsp::proto::STREAM_DATA && socketReply.has_data()) {
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

void RSPClient::handleNameServiceReply(const rsp::proto::RSPMessage& message) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (nameReplyPending_) {
        nameReplyMessage_ = message;
        nameReplyPending_ = false;
    }
    stateChanged_.notify_all();
}

void RSPClient::runNativeStreamBridge(const rsp::GUID& socketId,
                                      const std::shared_ptr<NativeStreamBridgeState>& bridgeState) {
    std::vector<uint8_t> buffer(4096);
    while (!bridgeState->stopping.load()) {
        const int bytesRead = rsp::os::recvSocket(bridgeState->bridgeSocket,
                                                  buffer.data(),
                                                  static_cast<uint32_t>(buffer.size()));
        if (bytesRead <= 0) {
            break;
        }

        const std::string payload(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(bytesRead));
        if (!streamSend(socketId, payload)) {
            break;
        }
    }

    bridgeState->stopping.store(true);
    rsp::os::closeSocket(bridgeState->bridgeSocket);
    if (!bridgeState->remoteClosed.load()) {
        streamClose(socketId);
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
            !reply->has_new_stream_id()) {
            if (reply->error() == rsp::proto::STREAM_CLOSED) {
                break;
            }

            continue;
        }

        const auto socketId = fromProtoStreamId(reply->new_stream_id());
        if (!socketId.has_value()) {
            continue;
        }

        const auto bridgeSocket = rsp::os::connectLocalListenerSocket(bridgeState->localEndpoint);
        if (!rsp::os::isValidSocket(bridgeSocket)) {
            streamClose(*socketId);
            break;
        }

        const auto socketBridge = attachNativeStreamBridge(*socketId, bridgeSocket);
        startNativeStreamBridgeWorker(*socketId, socketBridge);
    }

    streamClose(bridgeState->listenSocketId);
}

void RSPClient::stopNativeSocketBridges() {
    std::vector<std::shared_ptr<NativeStreamBridgeState>> nativeSocketBridges;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (auto& [_, bridgeState] : nativeStreamBridges_) {
            nativeSocketBridges.push_back(bridgeState);
        }
        nativeStreamBridges_.clear();
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
    std::map<rsp::GUID, std::shared_ptr<NativeStreamBridgeState>> nativeSocketBridges;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (auto iterator = nativeStreamBridges_.begin(); iterator != nativeStreamBridges_.end();) {
            const auto routeIterator = streamRoutes_.find(iterator->first);
            if (routeIterator != streamRoutes_.end() && routeIterator->second == nodeId) {
                nativeSocketBridges.emplace(iterator->first, iterator->second);
                iterator = nativeStreamBridges_.erase(iterator);
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

rsp::proto::Uuid RSPClient::toProtoUuid(const rsp::GUID& guid) {
    return ::rsp::client::toProtoUuid(guid);
}

rsp::proto::StreamID RSPClient::toProtoStreamId(const rsp::GUID& socketId) {
    rsp::proto::StreamID protoSocketId;
    std::string value(16, '\0');
    const uint64_t high = socketId.high();
    const uint64_t low = socketId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoSocketId.set_value(value);
    return protoSocketId;
}

std::optional<rsp::GUID> RSPClient::fromProtoStreamId(const rsp::proto::StreamID& socketId) {
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