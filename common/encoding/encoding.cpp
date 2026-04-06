#include "common/encoding/encoding.hpp"

#include "common/base_types.hpp"
#include "common/ping_trace.hpp"
#include "os/os_random.hpp"

#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

namespace rsp::encoding {

std::string toProtoNodeIdValue(const rsp::NodeID& nodeId);
std::string classifySendPrefix(const rsp::proto::RSPMessage& message, const rsp::KeyPair& localKeyPair);
std::string classifyReadPrefix(const rsp::proto::RSPMessage& message, const rsp::KeyPair& localKeyPair);
void recordClassifiedEvent(const rsp::proto::RSPMessage& message,
                           const rsp::KeyPair& localKeyPair,
                           const std::string& suffix,
                           bool isSendPath);

namespace {

class EncodingOutgoingQueue : public rsp::RSPMessageQueue {
public:
    explicit EncodingOutgoingQueue(Encoding& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override {
        owner_.dispatchSend(message);
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
    }

private:
    Encoding& owner_;
};

}  // namespace

namespace {

constexpr uint32_t kNonceLength = 16;

Buffer serializeUnsignedMessage(const rsp::proto::RSPMessage& message) {
    rsp::proto::RSPMessage unsignedMessage = message;
    unsignedMessage.clear_signature();

    std::string payload;
    if (!unsignedMessage.SerializeToString(&payload)) {
        throw std::runtime_error("failed to serialize unsigned authentication message");
    }

    if (payload.empty()) {
        return Buffer();
    }

    return Buffer(reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()));
}

std::string randomNonceBytes() {
    std::string nonce(kNonceLength, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(nonce.data()), kNonceLength);
    return nonce;
}

bool validateReceivedIdentity(const rsp::proto::RSPMessage& identityMessage,
                              const std::string& expectedNonce,
                              rsp::NodeID& peerNodeId) {
    if (!identityMessage.has_identity() || !identityMessage.has_signature()) {
        return false;
    }

    if (identityMessage.identity().nonce().value() != expectedNonce) {
        return false;
    }

    try {
        rsp::KeyPair peerKey = rsp::KeyPair::fromPublicKey(identityMessage.identity().public_key());
        if (!peerKey.verifyBlock(serializeUnsignedMessage(identityMessage), identityMessage.signature())) {
            return false;
        }

        peerNodeId = peerKey.nodeID();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

Encoding::Encoding(rsp::transport::ConnectionHandle connection, rsp::MessageQueueHandle receivedMessages, rsp::KeyPair localKeyPair)
    : connection_(std::move(connection)),
      receivedMessages_(std::move(receivedMessages)),
      outgoingMessages_(std::make_shared<EncodingOutgoingQueue>(*this)),
    localKeyPair_(std::move(localKeyPair)),
    peerNodeId_(std::nullopt),
      running_(false) {
    outgoingMessages_->setWorkerCount(1);
}

Encoding::~Encoding() {
    stop();
}

bool Encoding::start() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (running_ || connection_ == nullptr || receivedMessages_ == nullptr || outgoingMessages_ == nullptr ||
            !localKeyPair_.isValid() || !peerNodeId_.has_value()) {
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    running_ = true;
    outgoingMessages_->start();
    readThread_ = std::thread(&Encoding::readLoop, this);
    return true;
}

void Encoding::stop() {
    rsp::transport::ConnectionHandle connection;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!running_ && !readThread_.joinable()) {
            return;
        }

        running_ = false;
        connection = connection_;
    }

    if (connection != nullptr) {
        connection->close();
    }

    if (outgoingMessages_ != nullptr) {
        outgoingMessages_->stop();
    }

    if (readThread_.joinable() && readThread_.get_id() != std::this_thread::get_id()) {
        readThread_.join();
    }
}

bool Encoding::send(const rsp::proto::RSPMessage& message) {
    return queueSend(message);
}

rsp::MessageQueueHandle Encoding::outgoingMessages() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return outgoingMessages_;
}

rsp::transport::ConnectionHandle Encoding::connection() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return connection_;
}

void Encoding::enqueueReceived(rsp::proto::RSPMessage message) const {
    recordClassifiedEvent(message, localKeyPair_, "received_queue_enqueued", false);
    if (receivedMessages_ != nullptr) {
        receivedMessages_->push(std::move(message));
    }
}

rsp::proto::RSPMessage Encoding::normalizeOutgoingMessage(rsp::proto::RSPMessage message) const {
    if (!message.has_destination() && message.has_challenge_request()) {
        message.clear_signature();
    }

    return message;
}

bool Encoding::queueSend(rsp::proto::RSPMessage message) const {
    const auto queue = outgoingMessages();
    return queue != nullptr && queue->push(normalizeOutgoingMessage(std::move(message)));
}

bool Encoding::dispatchSend(const rsp::proto::RSPMessage& message) {
    recordClassifiedEvent(message, localKeyPair_, "send_worker_start", true);
    std::lock_guard<std::mutex> lock(sendMutex_);
    return writeMessage(message);
}

std::optional<rsp::NodeID> Encoding::peerNodeID() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return peerNodeId_;
}

void Encoding::setPeerNodeID(const rsp::NodeID& nodeId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (peerNodeId_.has_value() && peerNodeId_.value() != nodeId) {
        throw std::logic_error("peer NodeID is already established for this encoding");
    }

    peerNodeId_ = nodeId;
}

const rsp::KeyPair& Encoding::localKeyPair() const {
    return localKeyPair_;
}

bool Encoding::performInitialIdentityExchange() {
    const auto activeConnection = connection();
    if (activeConnection == nullptr || !localKeyPair_.isValid()) {
        return false;
    }

    rsp::proto::RSPMessage challengeRequest;
    challengeRequest.mutable_challenge_request()->mutable_nonce()->set_value(randomNonceBytes());
    const std::string localChallengeNonce = challengeRequest.challenge_request().nonce().value();

    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (!writeMessage(normalizeOutgoingMessage(std::move(challengeRequest)))) {
            return false;
        }
    }

    bool peerIdentityReceived = false;
    bool localIdentitySent = false;
    bool peerChallengeReceived = false;

    while (!peerIdentityReceived || !localIdentitySent) {
        rsp::proto::RSPMessage incomingMessage;
        if (!readMessage(incomingMessage)) {
            return false;
        }

        if (incomingMessage.has_challenge_request()) {
            if (incomingMessage.has_destination() || incomingMessage.has_signature() || peerChallengeReceived ||
                incomingMessage.challenge_request().nonce().value().size() != kNonceLength) {
                return false;
            }

            rsp::proto::RSPMessage identityMessage;
            identityMessage.mutable_identity()->mutable_nonce()->CopyFrom(incomingMessage.challenge_request().nonce());
            *identityMessage.mutable_identity()->mutable_public_key() = localKeyPair_.publicKey();
            *identityMessage.mutable_signature() = localKeyPair_.signBlock(serializeUnsignedMessage(identityMessage));

            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                if (!writeMessage(identityMessage)) {
                    return false;
                }
            }

            peerChallengeReceived = true;
            localIdentitySent = true;
            continue;
        }

        if (incomingMessage.has_identity()) {
            if (peerIdentityReceived) {
                return false;
            }

            rsp::NodeID peerNodeId(0, 0);
            if (!validateReceivedIdentity(incomingMessage, localChallengeNonce, peerNodeId)) {
                return false;
            }

            setPeerNodeID(peerNodeId);
            peerIdentityReceived = true;
            continue;
        }

        return false;
    }

    return true;
}

void Encoding::readLoop() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!running_) {
                break;
            }
        }

        rsp::proto::RSPMessage message;
        if (!readMessage(message)) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            running_ = false;
            break;
        }

        recordClassifiedEvent(message, localKeyPair_, "read_complete", false);

        enqueueReceived(std::move(message));
    }
}

std::string toProtoNodeIdValue(const rsp::NodeID& nodeId) {
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    return value;
}

std::string classifySendPrefix(const rsp::proto::RSPMessage& message, const rsp::KeyPair& localKeyPair) {
    const std::string localNodeId = toProtoNodeIdValue(localKeyPair.nodeID());

    if (message.has_ping_request()) {
        if (message.source().value() == localNodeId) {
            return "source_request";
        }

        return "rm_forward_request";
    }

    if (message.has_ping_reply() && message.source().value() == localNodeId) {
        return "destination_reply";
    }

    return "";
}

std::string classifyReadPrefix(const rsp::proto::RSPMessage& message, const rsp::KeyPair& localKeyPair) {
    const std::string localNodeId = toProtoNodeIdValue(localKeyPair.nodeID());

    if (message.has_ping_request()) {
        if (message.has_destination() && message.destination().value() == localNodeId) {
            return "destination_request";
        }

        return "rm_request";
    }

    if (message.has_ping_reply() && message.has_destination() && message.destination().value() == localNodeId) {
        return "source_reply";
    }

    return "";
}

void recordClassifiedEvent(const rsp::proto::RSPMessage& message,
                           const rsp::KeyPair& localKeyPair,
                           const std::string& suffix,
                           bool isSendPath) {
    if (!rsp::ping_trace::isEnabled()) {
        return;
    }

    const std::string prefix = isSendPath ? classifySendPrefix(message, localKeyPair)
                                          : classifyReadPrefix(message, localKeyPair);
    if (!prefix.empty()) {
        rsp::ping_trace::recordForMessage(message, prefix + "_" + suffix);
    }
}
}  // namespace rsp::encoding