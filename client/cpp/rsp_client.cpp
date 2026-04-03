#include "client/cpp/rsp_client.hpp"

#include "common/base_types.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <utility>

namespace rsp::client {

class ClientApiIncomingMessageQueue : public rsp::RSPMessageQueue {
public:
    explicit ClientApiIncomingMessageQueue(RSPClient& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override {
        owner_.dispatchIncomingMessage(std::move(message));
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
        std::cerr << "RSPClient incoming message queue dropped message because the queue is full" << std::endl;
    }

private:
    RSPClient& owner_;
};

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

}  // namespace

RSPClient::Ptr RSPClient::create() {
    return Ptr(new RSPClient(KeyPair::generateP256()));
}

RSPClient::Ptr RSPClient::create(KeyPair keyPair) {
    return Ptr(new RSPClient(std::move(keyPair)));
}

RSPClient::RSPClient(KeyPair keyPair)
    : messageClient_(RSPClientMessage::create(std::move(keyPair))),
      incomingMessages_(std::make_shared<ClientApiIncomingMessageQueue>(*this)) {
    incomingMessages_->setWorkerCount(1);
    incomingMessages_->start();
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

    if (incomingMessages_ != nullptr) {
        incomingMessages_->stop();
    }
}

int RSPClient::run() const {
    return 0;
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
    const uint32_t sequence = [this, &nonce, &nodeId]() {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const uint32_t nextSequence = nextPingSequence_++;
        pendingPings_.emplace(nonce, PendingPingState{nodeId, nextSequence, false});
        return nextSequence;
    }();

    rsp::proto::RSPMessage pingRequest;
    *pingRequest.mutable_source() = toProtoNodeId(messageClient_->nodeId());
    *pingRequest.mutable_destination() = toProtoNodeId(nodeId);
    pingRequest.mutable_ping_request()->mutable_nonce()->set_value(nonce);
    pingRequest.mutable_ping_request()->set_sequence(sequence);
    *pingRequest.mutable_ping_request()->mutable_time_sent() = toProtoDateTime(rsp::DateTime());

    if (!messageClient_->send(pingRequest)) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingPings_.erase(nonce);
        return false;
    }

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
            if (incomingMessages_ == nullptr || !incomingMessages_->push(std::move(message))) {
                std::cerr << "RSPClient failed to enqueue inbound message for API handling" << std::endl;
            }
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void RSPClient::dispatchIncomingMessage(rsp::proto::RSPMessage message) {
    if (message.has_ping_request() && shouldHandleLocally(message)) {
        handlePingRequest(message);
        return;
    }

    if (message.has_ping_reply()) {
        handlePingReply(message);
    }
}

bool RSPClient::shouldHandleLocally(const rsp::proto::RSPMessage& message) const {
    if (!message.has_destination()) {
        return true;
    }

    return message.destination().value() == toProtoNodeId(messageClient_->nodeId()).value();
}

void RSPClient::handlePingRequest(const rsp::proto::RSPMessage& message) {
    rsp::proto::RSPMessage reply;
    *reply.mutable_source() = toProtoNodeId(messageClient_->nodeId());
    if (message.has_source()) {
        *reply.mutable_destination() = message.source();
    }

    auto* pingReply = reply.mutable_ping_reply();
    pingReply->mutable_nonce()->CopyFrom(message.ping_request().nonce());
    pingReply->set_sequence(message.ping_request().sequence());
    pingReply->mutable_time_sent()->CopyFrom(message.ping_request().time_sent());
    *pingReply->mutable_time_replied() = toProtoDateTime(rsp::DateTime());

    if (!messageClient_->send(reply)) {
        std::cerr << "RSPClient failed to send ping reply" << std::endl;
    }
}

void RSPClient::handlePingReply(const rsp::proto::RSPMessage& message) {
    if (!message.ping_reply().has_nonce()) {
        return;
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
    stateChanged_.notify_all();
}

}  // namespace rsp::client