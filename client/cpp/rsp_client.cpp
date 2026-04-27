#include "client/cpp/rsp_client.hpp"

#include "common/base_types.hpp"
#include "common/endorsement/endorsement.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "common/ping_trace.hpp"
#include "common/service_message.hpp"

#include "logging/logging.pb.h"
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

rsp::proto::StreamID toProtoStreamId(const rsp::GUID& socketId) {
    rsp::proto::StreamID protoSocketId;
    std::string value(16, '\0');
    const uint64_t high = socketId.high();
    const uint64_t low = socketId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoSocketId.set_value(value);
    return protoSocketId;
}

std::optional<rsp::GUID> fromProtoStreamId(const rsp::proto::StreamID& socketId) {
    if (socketId.value().size() != 16) {
        return std::nullopt;
    }
    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, socketId.value().data(), sizeof(high));
    std::memcpy(&low, socketId.value().data() + sizeof(high), sizeof(low));
    return rsp::GUID(high, low);
}

bool isNameServiceReply(const rsp::proto::RSPMessage& message) {
    return rsp::hasServiceMessage<rsp::proto::NameCreateReply>(message) ||
           rsp::hasServiceMessage<rsp::proto::NameReadReply>(message) ||
           rsp::hasServiceMessage<rsp::proto::NameUpdateReply>(message) ||
           rsp::hasServiceMessage<rsp::proto::NameDeleteReply>(message) ||
           rsp::hasServiceMessage<rsp::proto::NameQueryReply>(message) ||
           rsp::hasServiceMessage<rsp::proto::NameRefreshReply>(message);
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

static StreamStatus streamStatusFromProto(rsp::proto::STREAM_STATUS s) {
    return static_cast<StreamStatus>(static_cast<int>(s));
}

static StreamResult streamResultFromReply(const rsp::proto::StreamReply& reply,
                                          const rsp::GUID& fallbackStreamId) {
    StreamResult r;
    r.status = streamStatusFromProto(reply.error());
    if (reply.has_stream_id()) {
        const auto id = fromProtoStreamId(reply.stream_id());
        r.streamId = id.value_or(fallbackStreamId);
    } else {
        r.streamId = fallbackStreamId;
    }
    if (reply.has_new_stream_id()) {
        const auto nid = fromProtoStreamId(reply.new_stream_id());
        if (nid.has_value()) {
            r.newStreamId = *nid;
            r.hasNewStreamId = true;
        }
    }
    if (reply.has_data()) {
        r.data = reply.data();
    }
    if (reply.has_message()) {
        r.message = reply.message();
    }
    if (reply.has_stream_error_code()) {
        r.errorCode = reply.stream_error_code();
    }
    return r;
}

static NameResult::Status nameStatusFromProto(rsp::proto::NAME_STATUS s) {
    return static_cast<NameResult::Status>(static_cast<int>(s));
}

static NameRecord nameRecordFromProto(const rsp::proto::NameRecord& r) {
    NameRecord rec;
    rec.name = r.name();
    if (r.has_owner() && r.owner().value().size() == 16) {
        uint64_t h = 0, l = 0;
        std::memcpy(&h, r.owner().value().data(), 8);
        std::memcpy(&l, r.owner().value().data() + 8, 8);
        rec.owner = rsp::NodeID(h, l);
    }
    if (r.has_type() && r.type().value().size() == 16) {
        uint64_t h = 0, l = 0;
        std::memcpy(&h, r.type().value().data(), 8);
        std::memcpy(&l, r.type().value().data() + 8, 8);
        rec.type = rsp::GUID(h, l);
    }
    if (r.has_value() && r.value().value().size() == 16) {
        uint64_t h = 0, l = 0;
        std::memcpy(&h, r.value().value().data(), 8);
        std::memcpy(&l, r.value().value().data() + 8, 8);
        rec.value = rsp::GUID(h, l);
    }
    if (r.has_expires_at()) {
        rec.expiresAt = rsp::DateTime::fromMillisecondsSinceEpoch(r.expires_at().milliseconds_since_epoch());
    }
    return rec;
}

static DiscoveredService discoveredServiceFromProto(const rsp::proto::DiscoveredService& svc) {
    DiscoveredService ds;
    if (svc.has_node_id() && svc.node_id().value().size() == 16) {
        uint64_t h = 0, l = 0;
        std::memcpy(&h, svc.node_id().value().data(), 8);
        std::memcpy(&l, svc.node_id().value().data() + 8, 8);
        ds.nodeId = rsp::NodeID(h, l);
    }
    if (svc.has_schema()) {
        ds.protoFileName = svc.schema().proto_file_name();
        for (const auto& url : svc.schema().accepted_type_urls()) {
            ds.acceptedTypeUrls.push_back(url);
        }
    }
    return ds;
}

void removeBufferedReply(std::deque<StreamResult>& pendingResults, const StreamResult& result) {
    for (auto it = pendingResults.begin(); it != pendingResults.end(); ++it) {
        if (it->streamId == result.streamId && it->status == result.status) {
            pendingResults.erase(it);
            return;
        }
    }
}

static std::optional<NameResult> sendNameRequestImpl(
        rsp::NodeID /*nodeId*/,
        const rsp::proto::RSPMessage& request,
        std::mutex& stateMutex,
        std::condition_variable& stateChanged,
        bool& nameReplyPending,
        std::optional<NameResult>& nameReplyResult,
        const bool& stopping,
        RSPClientMessage::Ptr& messageClient) {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        nameReplyPending = true;
        nameReplyResult.reset();
    }

    if (!messageClient->send(request)) {
        std::lock_guard<std::mutex> lock(stateMutex);
        nameReplyPending = false;
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lock(stateMutex);
    const bool replied = stateChanged.wait_for(lock, std::chrono::seconds(5), [&]() {
        return stopping || !nameReplyPending;
    });

    if (!replied || stopping || !nameReplyResult.has_value()) {
        nameReplyPending = false;
        return std::nullopt;
    }

    auto result = nameReplyResult;
    nameReplyResult.reset();
    return result;
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
        std::lock_guard<std::mutex> lock(refreshMutex_);
        refreshStopping_ = true;
    }
    refreshCv_.notify_all();
    if (refreshThread_.joinable()) {
        refreshThread_.join();
    }

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
    std::unique_lock<std::mutex> lock(stateMutex_);
    stateChanged_.wait(lock, [this] { return stopping_; });
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

    if (message.has_log_subscribe_reply()) {
        return true;
    }

    if (message.has_log_record()) {
        handleLogRecord(message);
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

std::optional<RSPClient::ClientConnectionID> RSPClient::connectToResourceManager(const std::string& transport,
                                                                                   const std::string& encoding) {
    return messageClient_->connectToResourceManager(transport, encoding);
}

void RSPClient::enableReconnect(ClientConnectionID connectionId,
                                std::function<void(ClientConnectionID)> onReconnected) {
    // Wrap the caller's callback so that after each reconnect, we also renew
    // the RM log subscriptions (NodeConnected/Disconnected events) and wake
    // the refresh thread to re-register any pending names.
    auto wrappedCallback = [this, userCallback = std::move(onReconnected)](ClientConnectionID id) {
        {
            std::lock_guard<std::mutex> lock(refreshMutex_);
            sendLogSubscribeToRM();
            refreshCv_.notify_all();
        }
        if (userCallback) {
            userCallback(id);
        }
    };
    messageClient_->enableReconnect(connectionId, std::move(wrappedCallback));
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

std::optional<EndorsementResult> RSPClient::beginEndorsementRequest(
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

        if (!repairedUnknownIdentity && reply->status == EndorsementResult::Status::UnknownIdentity) {
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

std::optional<EndorsementResult> RSPClient::waitForPendingEndorsement(const std::string& pendingKey) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool replied = stateChanged_.wait_for(lock, std::chrono::seconds(5), [this, &pendingKey]() {
        const auto iterator = pendingEndorsements_.find(pendingKey);
        return stopping_ || (iterator != pendingEndorsements_.end() && iterator->second.completed);
    });
    const auto iterator = pendingEndorsements_.find(pendingKey);
    if (!replied || stopping_ || iterator == pendingEndorsements_.end()) {
        return std::nullopt;
    }

    auto result = iterator->second.result;
    iterator->second.completed = false;
    iterator->second.result.reset();
    return result;
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

std::optional<ResourceQueryResult> RSPClient::resourceList(
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

std::optional<NameResult> RSPClient::nameCreate(
        rsp::NodeID nodeId, const std::string& name,
        rsp::NodeID owner, const rsp::GUID& type, const rsp::GUID& value) {
    rsp::proto::NameCreateRequest req;
    auto* record = req.mutable_record();
    record->set_name(name);
    *record->mutable_owner() = toProtoNodeId(owner);
    *record->mutable_type() = toProtoUuid(type);
    *record->mutable_value() = toProtoUuid(value);
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::packServiceMessage(request, req);
    return sendNameRequestImpl(nodeId, request, stateMutex_, stateChanged_,
        nameReplyPending_, nameReplyResult_, stopping_, messageClient_);
}

std::optional<NameResult> RSPClient::nameRead(
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
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::packServiceMessage(request, req);
    return sendNameRequestImpl(nodeId, request, stateMutex_, stateChanged_,
        nameReplyPending_, nameReplyResult_, stopping_, messageClient_);
}

std::optional<NameResult> RSPClient::nameUpdate(
        rsp::NodeID nodeId, const std::string& name,
        rsp::NodeID owner, const rsp::GUID& type, const rsp::GUID& newValue) {
    rsp::proto::NameUpdateRequest req;
    req.set_name(name);
    *req.mutable_owner() = toProtoNodeId(owner);
    *req.mutable_type() = toProtoUuid(type);
    *req.mutable_new_value() = toProtoUuid(newValue);
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::packServiceMessage(request, req);
    return sendNameRequestImpl(nodeId, request, stateMutex_, stateChanged_,
        nameReplyPending_, nameReplyResult_, stopping_, messageClient_);
}

std::optional<NameResult> RSPClient::nameDelete(
        rsp::NodeID nodeId, const std::string& name,
        rsp::NodeID owner, const rsp::GUID& type) {
    rsp::proto::NameDeleteRequest req;
    req.set_name(name);
    *req.mutable_owner() = toProtoNodeId(owner);
    *req.mutable_type() = toProtoUuid(type);
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::packServiceMessage(request, req);
    return sendNameRequestImpl(nodeId, request, stateMutex_, stateChanged_,
        nameReplyPending_, nameReplyResult_, stopping_, messageClient_);
}

std::optional<NameResult> RSPClient::nameQuery(
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
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::packServiceMessage(request, req);
    return sendNameRequestImpl(nodeId, request, stateMutex_, stateChanged_,
        nameReplyPending_, nameReplyResult_, stopping_, messageClient_);
}

std::optional<NameResult> RSPClient::nameRefresh(
        rsp::NodeID nodeId, const std::string& name,
        rsp::NodeID owner, const rsp::GUID& type) {
    rsp::proto::NameRefreshRequest req;
    req.set_name(name);
    *req.mutable_owner() = toProtoNodeId(owner);
    *req.mutable_type() = toProtoUuid(type);
    rsp::proto::RSPMessage request;
    *request.mutable_destination() = toProtoNodeId(nodeId);
    rsp::packServiceMessage(request, req);
    return sendNameRequestImpl(nodeId, request, stateMutex_, stateChanged_,
        nameReplyPending_, nameReplyResult_, stopping_, messageClient_);
}

std::optional<rsp::NodeID> RSPClient::nameResolve(
        rsp::NodeID nsNodeId,
        const std::string& name,
        const std::optional<rsp::GUID>& type) {
    const auto result = nameQuery(nsNodeId, name, std::nullopt, type);
    if (!result.has_value() || result->records.empty()) {
        return std::nullopt;
    }
    for (const auto& rec : result->records) {
        if (ping(rec.owner)) {
            return rec.owner;
        }
    }
    return std::nullopt;
}

std::optional<NameResult> RSPClient::registerNameWithRefresh(
        rsp::NodeID nsNodeId, const std::string& name,
        rsp::NodeID owner, const rsp::GUID& type, const rsp::GUID& value) {
    auto result = nameCreate(nsNodeId, name, owner, type, value);
    {
        std::lock_guard<std::mutex> lock(refreshMutex_);
        refreshRegistrations_.push_back({nsNodeId, name, owner, type, value});
        if (!result.has_value() || result->status != NameResult::Status::Success) {
            // NS may be down; flag for immediate retry when NodeConnectedEvent arrives.
            pendingReregistrations_.insert(nsNodeId);
        }
        if (!refreshThread_.joinable()) {
            refreshStopping_ = false;
            refreshThread_ = std::thread([this] { runRefreshThread(); });
        }
        if (!logSubActive_) {
            sendLogSubscribeToRM();
            logSubActive_ = true;
        }
    }
    return result;
}

void RSPClient::runRefreshThread() {
    static constexpr auto kRefreshInterval = std::chrono::seconds(150); // TTL/2
    static constexpr auto kRetryInterval   = std::chrono::seconds(15);  // retry after failure
    while (true) {
        std::vector<RefreshEntry> entries;
        std::set<rsp::NodeID> reregister;
        {
            std::unique_lock<std::mutex> lock(refreshMutex_);
            const bool hasFailed = !failedRegistrations_.empty();
            refreshCv_.wait_for(lock, hasFailed ? kRetryInterval : kRefreshInterval, [this] {
                return refreshStopping_ || !pendingReregistrations_.empty();
            });
            if (refreshStopping_) break;
            entries = refreshRegistrations_;
            reregister = std::move(pendingReregistrations_);
            pendingReregistrations_.clear();
            // Treat previously-failed entries as pending re-registration.
            for (const auto& nsNodeId : failedRegistrations_) {
                reregister.insert(nsNodeId);
            }
            failedRegistrations_.clear();
            sendLogSubscribeToRM(); // renew subscription periodically
        }
        for (const auto& entry : entries) {
            if (reregister.count(entry.nsNodeId)) {
                const auto result = nameCreate(entry.nsNodeId, entry.name, entry.owner, entry.type, entry.value);
                if (!result.has_value() || result->status == NameResult::Status::Error) {
                    std::lock_guard<std::mutex> lock(refreshMutex_);
                    failedRegistrations_.insert(entry.nsNodeId);
                }
            } else {
                const auto result = nameRefresh(entry.nsNodeId, entry.name, entry.owner, entry.type);
                if (!result.has_value() || result->status == NameResult::Status::NotFound) {
                    // Record was lost (NS restarted); re-create it.
                    const auto createResult = nameCreate(entry.nsNodeId, entry.name, entry.owner, entry.type, entry.value);
                    if (!createResult.has_value() || createResult->status == NameResult::Status::Error) {
                        std::lock_guard<std::mutex> lock(refreshMutex_);
                        failedRegistrations_.insert(entry.nsNodeId);
                    }
                }
            }
        }
    }
}

void RSPClient::sendLogSubscribeToRM() {
    const auto connIds = messageClient_->connectionIds();
    for (const auto& connId : connIds) {
        const auto rmNodeId = messageClient_->peerNodeID(connId);
        if (!rmNodeId.has_value()) {
            continue;
        }
        const auto sendSubscription = [&](const std::string& typeUrl) {
            rsp::proto::RSPMessage request;
            *request.mutable_source() = toProtoNodeId(messageClient_->nodeId());
            *request.mutable_destination() = toProtoNodeId(*rmNodeId);
            auto* sub = request.mutable_log_subscribe_request();
            sub->set_payload_type_url(typeUrl);
            sub->mutable_filter()->mutable_field_exists()->mutable_path()->add_segments("node_id");
            sub->set_duration_ms(300000); // 5 minutes
            messageClient_->send(request);
        };
        sendSubscription("type.rsp/rsp.proto.NodeConnectedEvent");
        sendSubscription("type.rsp/rsp.proto.NodeDisconnectedEvent");
    }
}

void RSPClient::watchNodeConnectedEvents(std::function<void(rsp::NodeID)> callback) {
    std::lock_guard<std::mutex> lock(refreshMutex_);
    nodeConnectedCallback_ = std::move(callback);
    if (!refreshThread_.joinable()) {
        refreshStopping_ = false;
        refreshThread_ = std::thread([this] { runRefreshThread(); });
    }
    if (!logSubActive_) {
        sendLogSubscribeToRM();
        logSubActive_ = true;
    }
}

void RSPClient::watchNodeLifecycle(NodeLifecycleCallbacks callbacks) {
    std::lock_guard<std::mutex> lock(refreshMutex_);
    lifecycleCallbacks_ = std::move(callbacks);
    if (!refreshThread_.joinable()) {
        refreshStopping_ = false;
        refreshThread_ = std::thread([this] { runRefreshThread(); });
    }
    if (!logSubActive_) {
        sendLogSubscribeToRM();
        logSubActive_ = true;
    }
}

void RSPClient::handleLogRecord(const rsp::proto::RSPMessage& message) {
    if (!message.has_log_record() || !message.log_record().has_payload()) {
        return;
    }
    const auto& payload = message.log_record().payload();

    if (payload.type_url() == "type.rsp/rsp.proto.NodeConnectedEvent") {
        rsp::proto::NodeConnectedEvent event;
        if (!event.ParseFromString(payload.value())) return;
        const auto nodeId = rsp::nodeIdFromSourceField(event.node_id());
        if (!nodeId.has_value()) return;

        const std::string newBootId = event.identity().has_boot_id()
            ? event.identity().boot_id().value()
            : std::string();

        std::function<void(rsp::NodeID)> unknownNodeCallback;
        std::function<void(rsp::NodeID)> lifecycleOnConnected;
        {
            std::lock_guard<std::mutex> lock(refreshMutex_);

            // NS re-registration logic: check if this node is a known NS.
            bool isKnownNS = false;
            for (const auto& entry : refreshRegistrations_) {
                if (entry.nsNodeId == *nodeId) { isKnownNS = true; break; }
            }
            if (!isKnownNS) {
                unknownNodeCallback = nodeConnectedCallback_;
            } else {
                const auto it = nsBootIds_.find(*nodeId);
                if (it != nsBootIds_.end() && it->second == newBootId && !newBootId.empty()) {
                    return; // same boot_id = transport reconnect, skip
                }
                nsBootIds_[*nodeId] = newBootId;
                pendingReregistrations_.insert(*nodeId);
                refreshCv_.notify_all();
            }

            // General lifecycle: fire onConnected if boot_id changed (or first connect).
            if (lifecycleCallbacks_.has_value() && lifecycleCallbacks_->onConnected) {
                const auto it = lifecycleBootIds_.find(*nodeId);
                if (it == lifecycleBootIds_.end() || it->second != newBootId || newBootId.empty()) {
                    lifecycleBootIds_[*nodeId] = newBootId;
                    lifecycleOnConnected = lifecycleCallbacks_->onConnected;
                }
            }
        }
        if (unknownNodeCallback) unknownNodeCallback(*nodeId);
        if (lifecycleOnConnected) lifecycleOnConnected(*nodeId);

    } else if (payload.type_url() == "type.rsp/rsp.proto.NodeDisconnectedEvent") {
        rsp::proto::NodeDisconnectedEvent event;
        if (!event.ParseFromString(payload.value())) return;
        const auto nodeId = rsp::nodeIdFromSourceField(event.node_id());
        if (!nodeId.has_value()) return;

        std::function<void(rsp::NodeID)> lifecycleOnDisconnected;
        {
            std::lock_guard<std::mutex> lock(refreshMutex_);
            // Clear boot_id so the next connect fires onConnected unconditionally.
            lifecycleBootIds_.erase(*nodeId);
            if (lifecycleCallbacks_.has_value() && lifecycleCallbacks_->onDisconnected) {
                lifecycleOnDisconnected = lifecycleCallbacks_->onDisconnected;
            }
        }
        if (lifecycleOnDisconnected) lifecycleOnDisconnected(*nodeId);
    }
}


std::optional<StreamResult> RSPClient::connectTCPEx(rsp::NodeID nodeId,
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

    StreamResult result = iterator->second.result;
    pendingConnects_.erase(iterator);
    if (result.status == StreamStatus::Success) {
        streamRoutes_[socketId] = nodeId;
    }

    return result;
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
    if (!reply.has_value() || reply->status != StreamStatus::Success) {
        return std::nullopt;
    }

    return reply->streamId;
}

std::optional<StreamResult> RSPClient::listenTCPEx(rsp::NodeID nodeId,
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

    StreamResult result = iterator->second.result;
    pendingListens_.erase(iterator);
    if (result.status == StreamStatus::Success) {
        streamRoutes_[socketId] = nodeId;
    }

    return result;
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
    if (!reply.has_value() || reply->status != StreamStatus::Success) {
        return std::nullopt;
    }

    return reply->streamId;
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

std::optional<StreamResult> RSPClient::acceptTCPEx(const rsp::GUID& listenSocketId,
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
        (reply->status == StreamStatus::Success || reply->status == StreamStatus::NewConnection) &&
        reply->hasNewStreamId) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        streamRoutes_[reply->newStreamId] = destination;
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
        (reply->status != StreamStatus::Success && reply->status != StreamStatus::NewConnection) ||
        !reply->hasNewStreamId) {
        return std::nullopt;
    }

    return reply->newStreamId;
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

std::optional<StreamResult> RSPClient::connectSshdEx(rsp::NodeID nodeId,
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

    StreamResult result = iterator->second.result;
    pendingConnects_.erase(iterator);
    if (result.status == StreamStatus::Success) {
        streamRoutes_[socketId] = nodeId;
    }

    return result;
}

std::optional<rsp::GUID> RSPClient::connectSshd(rsp::NodeID nodeId,
                                                  uint32_t timeoutMilliseconds,
                                                  bool asyncData,
                                                  bool shareSocket,
                                                  bool useSocket) {
    const auto reply = connectSshdEx(nodeId, timeoutMilliseconds, asyncData, shareSocket, useSocket);
    if (!reply.has_value() || reply->status != StreamStatus::Success) {
        return std::nullopt;
    }
    return reply->streamId;
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


std::optional<StreamResult> RSPClient::connectHttpEx(rsp::NodeID nodeId,
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

    StreamResult result = it->second.result;
    pendingConnects_.erase(it);
    if (result.status == StreamStatus::Success) {
        streamRoutes_[socketId] = nodeId;
    }

    return result;
}

std::optional<rsp::GUID> RSPClient::connectHttp(rsp::NodeID nodeId,
                                                  uint32_t timeoutMilliseconds,
                                                  bool asyncData,
                                                  bool shareSocket) {
    const auto reply = connectHttpEx(nodeId, timeoutMilliseconds, asyncData, shareSocket);
    if (!reply.has_value() || reply->status != StreamStatus::Success) {
        return std::nullopt;
    }

    return reply->streamId;
}

std::shared_ptr<RSPClient::NativeStreamBridgeState> RSPClient::attachNativeStreamBridge(
    const rsp::GUID& socketId,
    rsp::os::SocketHandle bridgeSocket) {
    auto bridgeState = std::make_shared<NativeStreamBridgeState>();
    bridgeState->bridgeSocket = bridgeSocket;

    std::deque<StreamResult> bufferedReplies;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        nativeStreamBridges_[socketId] = bridgeState;
        const auto queuedReplies = streamReplyQueues_.find(socketId);
        if (queuedReplies != streamReplyQueues_.end()) {
            bufferedReplies = std::move(queuedReplies->second);
            streamReplyQueues_.erase(queuedReplies);
            for (const auto& reply : bufferedReplies) {
                removeBufferedReply(pendingStreamResults_, reply);
            }
        }
    }

    for (const auto& reply : bufferedReplies) {
        if (reply.status == StreamStatus::Data && !reply.data.empty()) {
            if (!sendAllToSocket(bridgeState->bridgeSocket,
                                 reinterpret_cast<const uint8_t*>(reply.data.data()),
                                 reply.data.size())) {
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

    return reply->status == StreamStatus::Success;
}

std::optional<std::string> RSPClient::streamRecv(const rsp::GUID& socketId,
                                                 uint32_t maxBytes,
                                                 uint32_t waitMilliseconds) {
    const auto reply = streamRecvEx(socketId, maxBytes, waitMilliseconds);
    if (!reply.has_value()) {
        return std::nullopt;
    }

    if (reply->status != StreamStatus::Data && reply->status != StreamStatus::Success) {
        return std::nullopt;
    }

    return reply->data;
}

std::optional<StreamResult> RSPClient::streamRecvEx(const rsp::GUID& socketId,
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

std::optional<StreamResult> RSPClient::waitForStreamReply(const rsp::GUID& socketId) {
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

    const StreamResult reply = iterator->second.front();
    iterator->second.pop_front();
    awaitedStreamReplies_.erase(socketId);
    for (auto globalIterator = pendingStreamResults_.begin(); globalIterator != pendingStreamResults_.end(); ++globalIterator) {
        if (globalIterator->streamId == reply.streamId && globalIterator->status == reply.status) {
            pendingStreamResults_.erase(globalIterator);
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

        if (reply->status == StreamStatus::Success || reply->status == StreamStatus::Closed) {
            streamRoutes_.erase(socketId);
            return true;
        }

        if (reply->status != StreamStatus::Data &&
            reply->status != StreamStatus::Async &&
            reply->status != StreamStatus::NewConnection) {
            return false;
        }
    }

    streamRoutes_.erase(socketId);
    return true;
}

bool RSPClient::tryDequeueStreamResult(StreamResult& result) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (pendingStreamResults_.empty()) {
        return false;
    }

    result = pendingStreamResults_.front();
    pendingStreamResults_.pop_front();
    return true;
}

std::size_t RSPClient::pendingStreamReplyCount() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return pendingStreamResults_.size();
}

bool RSPClient::tryDequeueResourceAdvertisement(ResourceAdvertisement& advertisement) {
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

bool RSPClient::tryDequeueResourceQueryReply(ResourceQueryResult& reply) {
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

bool RSPClient::tryDequeueSchemaReply(SchemaInfo& reply) {
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
    for (const auto& schema : message.schema_reply().schemas()) {
        SchemaInfo info;
        info.protoFileName = schema.proto_file_name();
        info.descriptorSet = schema.proto_file_descriptor_set();
        for (const auto& url : schema.accepted_type_urls()) {
            info.acceptedTypeUrls.push_back(url);
        }
        info.schemaVersion = schema.schema_version();
        pendingSchemaReplies_.push_back(info);
    }
    stateChanged_.notify_all();
}

void RSPClient::handleResourceQueryReply(const rsp::proto::RSPMessage& message) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    ResourceQueryResult result;
    result.success = true;
    for (const auto& svc : message.resource_query_reply().services()) {
        result.services.push_back(discoveredServiceFromProto(svc));
    }
    if (resourceListPending_) {
        resourceListResult_ = result;
        resourceListPending_ = false;
    } else {
        pendingResourceQueryReplies_.push_back(result);
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
    EndorsementResult er;
    er.status = static_cast<EndorsementResult::Status>(static_cast<int>(done.status()));
    if (done.has_new_endorsement()) {
        er.newEndorsement = rsp::Endorsement::fromProto(done.new_endorsement());
    }
    iterator->second.result = er;
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
                    pendingIterator->second.result = streamResultFromReply(socketReply, *replySocketId);
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
                    pendingListenIterator->second.result = streamResultFromReply(socketReply, *replySocketId);
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
                streamReplyQueues_[*replySocketId].push_back(streamResultFromReply(socketReply, *replySocketId));
                if (awaitedIterator != awaitedStreamReplies_.end()) {
                    awaitedStreamReplies_.erase(awaitedIterator);
                    stateChanged_.notify_all();
                    return;
                }
            }
        }

        if (nativeSocketBridge == nullptr) {
            pendingStreamResults_.push_back(streamResultFromReply(socketReply, replySocketId.value_or(rsp::GUID{})));
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
    ResourceAdvertisement adv;
    for (const auto& svc : message.resource_advertisement().schemas()) {
        DiscoveredService ds;
        ds.protoFileName = svc.proto_file_name();
        for (const auto& url : svc.accepted_type_urls()) {
            ds.acceptedTypeUrls.push_back(url);
        }
        adv.services.push_back(ds);
    }
    pendingResourceAdvertisements_.push_back(adv);
    stateChanged_.notify_all();
}

void RSPClient::handleNameServiceReply(const rsp::proto::RSPMessage& message) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!nameReplyPending_) {
        stateChanged_.notify_all();
        return;
    }

    NameResult result;
    rsp::proto::NameCreateReply createReply;
    rsp::proto::NameReadReply readReply;
    rsp::proto::NameUpdateReply updateReply;
    rsp::proto::NameDeleteReply deleteReply;
    rsp::proto::NameQueryReply queryReply;
    rsp::proto::NameRefreshReply refreshReply;

    if (rsp::unpackServiceMessage(message, &createReply)) {
        result.status = nameStatusFromProto(createReply.status());
        if (createReply.has_message()) result.message = createReply.message();
    } else if (rsp::unpackServiceMessage(message, &readReply)) {
        result.status = nameStatusFromProto(readReply.status());
        for (const auto& r : readReply.records()) result.records.push_back(nameRecordFromProto(r));
    } else if (rsp::unpackServiceMessage(message, &updateReply)) {
        result.status = nameStatusFromProto(updateReply.status());
        if (updateReply.has_message()) result.message = updateReply.message();
    } else if (rsp::unpackServiceMessage(message, &deleteReply)) {
        result.status = nameStatusFromProto(deleteReply.status());
        if (deleteReply.has_message()) result.message = deleteReply.message();
    } else if (rsp::unpackServiceMessage(message, &queryReply)) {
        result.status = NameResult::Status::Success;
        for (const auto& r : queryReply.records()) result.records.push_back(nameRecordFromProto(r));
    } else if (rsp::unpackServiceMessage(message, &refreshReply)) {
        result.status = nameStatusFromProto(refreshReply.status());
        if (refreshReply.has_message()) result.message = refreshReply.message();
    }

    nameReplyResult_ = result;
    nameReplyPending_ = false;
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

        if (reply->status == StreamStatus::TimedOut) {
            continue;
        }

        if ((reply->status != StreamStatus::Success && reply->status != StreamStatus::NewConnection) ||
            !reply->hasNewStreamId) {
            if (reply->status == StreamStatus::Closed) {
                break;
            }

            continue;
        }

        const rsp::GUID socketId = reply->newStreamId;

        const auto bridgeSocket = rsp::os::connectLocalListenerSocket(bridgeState->localEndpoint);
        if (!rsp::os::isValidSocket(bridgeSocket)) {
            streamClose(socketId);
            break;
        }

        const auto socketBridge = attachNativeStreamBridge(socketId, bridgeSocket);
        startNativeStreamBridgeWorker(socketId, socketBridge);
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

}  // namespace rsp::client
