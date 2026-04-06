#include "client/cpp/rsp_client.hpp"

#include "common/base_types.hpp"
#include "common/ping_trace.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
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

}  // namespace rsp::client