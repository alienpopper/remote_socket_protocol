#include "common/node.hpp"

#include "common/base_types.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "os/os_random.hpp"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rsp {

class NodeInputQueue : public rsp::RSPMessageQueue {
public:
    explicit NodeInputQueue(RSPNode& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override;

    void handleQueueFull(size_t, size_t, const Message&) override {
        std::cerr << "RSPNode input queue dropped message because the queue is full" << std::endl;
    }

private:
    RSPNode& owner_;
};

class NodeOutputQueue : public rsp::RSPMessageQueue {
public:
    explicit NodeOutputQueue(RSPNode& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override {
        owner_.handleOutputMessage(std::move(message));
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
        std::cerr << "RSPNode output queue dropped message because the queue is full" << std::endl;
    }

private:
    RSPNode& owner_;
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

std::optional<rsp::NodeID> nodeIdFromIdentity(const rsp::proto::Identity& identity) {
    try {
        const rsp::KeyPair identityKey = rsp::KeyPair::fromPublicKey(identity.public_key());
        return identityKey.nodeID();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string randomChallengeNonce() {
    std::string nonce(16, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(nonce.data()), static_cast<uint32_t>(nonce.size()));
    return nonce;
}

std::string randomKeyId() {
    std::string keyId(16, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(keyId.data()), static_cast<uint32_t>(keyId.size()));
    return keyId;
}

bool isValidKeyId(const std::string& keyId) {
    return keyId.size() == 16;
}

uint64_t clampLifetimeMs(uint64_t requestedLifetimeMs, const rsp::RSPNode::AesNegotiationLimits& limits) {
    const uint64_t requested = requestedLifetimeMs == 0 ? limits.defaultLifetimeMs : requestedLifetimeMs;
    return std::clamp(requested, limits.minLifetimeMs, limits.maxLifetimeMs);
}

rsp::DateTime addMilliseconds(const rsp::DateTime& base, uint64_t milliseconds) {
    rsp::DateTime out = base;
    out += static_cast<double>(milliseconds) / 1000.0;
    return out;
}

std::optional<std::string> deriveSharedSecret(EVP_PKEY* localPrivateKey, EVP_PKEY* peerPublicKey) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new(localPrivateKey, nullptr), &EVP_PKEY_CTX_free);
    if (context == nullptr) {
        return std::nullopt;
    }

    if (EVP_PKEY_derive_init(context.get()) != 1) {
        return std::nullopt;
    }
    if (EVP_PKEY_derive_set_peer(context.get(), peerPublicKey) != 1) {
        return std::nullopt;
    }

    size_t secretLength = 0;
    if (EVP_PKEY_derive(context.get(), nullptr, &secretLength) != 1 || secretLength == 0) {
        return std::nullopt;
    }

    std::string secret(secretLength, '\0');
    if (EVP_PKEY_derive(context.get(), reinterpret_cast<uint8_t*>(secret.data()), &secretLength) != 1 ||
        secretLength == 0) {
        return std::nullopt;
    }

    secret.resize(secretLength);
    return secret;
}

rsp::Buffer deriveAesKeyMaterial(const std::string& sharedSecret,
                                 const std::string& keyId,
                                 const rsp::NodeID& firstNode,
                                 const rsp::NodeID& secondNode) {
    const std::string firstNodeBytes = toProtoNodeId(firstNode).value();
    const std::string secondNodeBytes = toProtoNodeId(secondNode).value();
    const std::string& lowerNodeBytes = firstNodeBytes <= secondNodeBytes ? firstNodeBytes : secondNodeBytes;
    const std::string& upperNodeBytes = firstNodeBytes <= secondNodeBytes ? secondNodeBytes : firstNodeBytes;
    static constexpr char kContext[] = "rsp-aes-negotiation-v1";

    rsp::Buffer keyMaterial(32);
    SHA256_CTX context{};
    SHA256_Init(&context);
    SHA256_Update(&context, sharedSecret.data(), sharedSecret.size());
    SHA256_Update(&context, keyId.data(), keyId.size());
    SHA256_Update(&context, lowerNodeBytes.data(), lowerNodeBytes.size());
    SHA256_Update(&context, upperNodeBytes.data(), upperNodeBytes.size());
    SHA256_Update(&context, kContext, sizeof(kContext) - 1);
    SHA256_Final(keyMaterial.data(), &context);
    return keyMaterial;
}

rsp::proto::RSPMessage makeAesKeyDropNotification(const rsp::NodeID& sourceNodeId,
                                                  const rsp::NodeID& destinationNodeId,
                                                  const std::string& keyId,
                                                  const std::string& reason) {
    rsp::proto::RSPMessage message;
    *message.mutable_source() = toProtoNodeId(sourceNodeId);
    *message.mutable_destination() = toProtoNodeId(destinationNodeId);
    auto* drop = message.mutable_aes_key_drop_notification();
    drop->mutable_key_id()->set_value(keyId);
    drop->set_reason(reason);
    return message;
}

rsp::proto::RSPMessage makeAesKeyNegotiationError(const rsp::NodeID& sourceNodeId,
                                                  const rsp::NodeID& destinationNodeId,
                                                  rsp::proto::KEY_NEGOTIATION_ERROR errorCode,
                                                  const std::optional<std::string>& keyId,
                                                  const std::string& errorMessage) {
    rsp::proto::RSPMessage message;
    *message.mutable_source() = toProtoNodeId(sourceNodeId);
    *message.mutable_destination() = toProtoNodeId(destinationNodeId);
    auto* error = message.mutable_aes_key_negotiation_error();
    error->set_error_code(errorCode);
    if (keyId.has_value()) {
        error->mutable_key_id()->set_value(*keyId);
    }
    error->set_message(errorMessage);
    return message;
}

rsp::proto::Identity sanitizedIdentityForCache(const rsp::proto::Identity& identity) {
    rsp::proto::Identity cachedIdentity;
    *cachedIdentity.mutable_public_key() = identity.public_key();
    return cachedIdentity;
}

}  // namespace

IdentityCache::IdentityCache(NodeID localNodeId, SendMessageCallback sendMessageCallback, size_t maximumEntries)
    : maximumEntries_(maximumEntries),
      localNodeId_(localNodeId),
      sendMessageCallback_(std::move(sendMessageCallback)) {
}

bool IdentityCache::observeMessage(const rsp::proto::RSPMessage& message) {
    bool cached = false;
    const auto sourceNodeId = rsp::senderNodeIdFromMessage(message);

    if (message.has_identity()) {
        if (cacheIdentity(message.identity(), sourceNodeId)) {
            cached = true;
        }
    }

    for (int i = 0; i < message.identities_size(); ++i) {
        if (cacheIdentity(message.identities(i), sourceNodeId)) {
            cached = true;
        }
    }

    return cached;
}

bool IdentityCache::sendChallengeRequest(const rsp::NodeID& nodeId) const {
    SendMessageCallback sendMessageCallback;
    std::optional<rsp::NodeID> localNodeId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sendMessageCallback = sendMessageCallback_;
        localNodeId = localNodeId_;
    }

    if (!sendMessageCallback || !localNodeId.has_value()) {
        return false;
    }

    rsp::proto::RSPMessage challengeRequest;
    *challengeRequest.mutable_source() = toProtoNodeId(*localNodeId);
    *challengeRequest.mutable_destination() = toProtoNodeId(nodeId);
    challengeRequest.mutable_challenge_request()->mutable_nonce()->set_value(randomChallengeNonce());
    return sendMessageCallback(std::move(challengeRequest));
}

IdentityCache::IdentityObservedCallbackToken IdentityCache::addIdentityObservedCallback(IdentityObservedCallback callback) {
    if (!callback) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const IdentityObservedCallbackToken token = nextIdentityObservedCallbackToken_++;
    identityObservedCallbacks_[token] = std::move(callback);
    return token;
}

bool IdentityCache::removeIdentityObservedCallback(IdentityObservedCallbackToken token) {
    std::lock_guard<std::mutex> lock(mutex_);
    return identityObservedCallbacks_.erase(token) > 0;
}

std::optional<rsp::proto::Identity> IdentityCache::get(const rsp::NodeID& nodeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = entries_.find(nodeId);
    if (iterator == entries_.end()) {
        return std::nullopt;
    }

    return iterator->second.identity;
}

bool IdentityCache::contains(const rsp::NodeID& nodeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(nodeId) != entries_.end();
}

size_t IdentityCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t IdentityCache::maximumEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maximumEntries_;
}

void IdentityCache::setMaximumEntries(size_t maximumEntries) {
    std::lock_guard<std::mutex> lock(mutex_);
    maximumEntries_ = maximumEntries;
    trimToLimitLocked();
}

bool IdentityCache::cacheIdentity(const rsp::proto::Identity& identity,
                                  const std::optional<rsp::NodeID>& sourceNodeId) {
    const auto derivedNodeId = nodeIdFromIdentity(identity);
    if (!derivedNodeId.has_value()) {
        return false;
    }

    const rsp::proto::Identity cachedIdentity = sanitizedIdentityForCache(identity);

    if (sourceNodeId.has_value() && *sourceNodeId != *derivedNodeId) {
        return false;
    }

    std::vector<IdentityObservedCallback> callbacksToNotify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = entries_.find(*derivedNodeId);
        if (existing != entries_.end()) {
            usageOrder_.erase(existing->second.usage);
            usageOrder_.push_front(*derivedNodeId);
            existing->second.identity = cachedIdentity;
            existing->second.usage = usageOrder_.begin();
        } else {
            usageOrder_.push_front(*derivedNodeId);
            entries_[*derivedNodeId] = CacheEntry{cachedIdentity, usageOrder_.begin()};
            trimToLimitLocked();
        }

        callbacksToNotify.reserve(identityObservedCallbacks_.size());
        for (const auto& [_, callback] : identityObservedCallbacks_) {
            if (callback) {
                callbacksToNotify.push_back(callback);
            }
        }
    }

    for (const auto& callback : callbacksToNotify) {
        callback(*derivedNodeId);
    }

    return true;
}

void IdentityCache::trimToLimitLocked() {
    while (entries_.size() > maximumEntries_) {
        const rsp::NodeID leastRecent = usageOrder_.back();
        usageOrder_.pop_back();
        entries_.erase(leastRecent);
    }
}

void NodeInputQueue::handleMessage(Message message, rsp::MessageQueueSharedState&) {
    if (message.has_log_subscribe_request()) {
        const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
        if (!requesterNodeId.has_value()) {
            return;
        }

        rsp::proto::RSPMessage response;
        *response.mutable_destination() = toProtoNodeId(*requesterNodeId);
        rsp::copyMessageTrace(message, response);
        *response.mutable_log_subscribe_reply() = owner_.loggingSubscriptions_.subscribe(
            *requesterNodeId, message.log_subscribe_request(), rsp::DateTime());

        const auto queue = owner_.outputQueue();
        if (queue == nullptr || !queue->push(std::move(response))) {
            std::cerr << "RSPNode failed to enqueue log subscribe reply on the output queue" << std::endl;
        }
        return;
    }

    if (message.has_log_unsubscribe_request()) {
        const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
        if (!requesterNodeId.has_value()) {
            return;
        }

        rsp::proto::RSPMessage response;
        *response.mutable_destination() = toProtoNodeId(*requesterNodeId);
        rsp::copyMessageTrace(message, response);
        *response.mutable_log_unsubscribe_reply() = owner_.loggingSubscriptions_.unsubscribe(
            *requesterNodeId, message.log_unsubscribe_request());

        const auto queue = owner_.outputQueue();
        if (queue == nullptr || !queue->push(std::move(response))) {
            std::cerr << "RSPNode failed to enqueue log unsubscribe reply on the output queue" << std::endl;
        }
        return;
    }

    if (owner_.handleNodeSpecificMessage(message)) {
        return;
    }

    if (message.has_challenge_request()) {
        const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
        if (!requesterNodeId.has_value()) {
            return;
        }

        if (!message.challenge_request().has_nonce() ||
            message.challenge_request().nonce().value().size() != 16) {
            return;
        }

        rsp::proto::RSPMessage identityReply;
        *identityReply.mutable_destination() = toProtoNodeId(*requesterNodeId);
        rsp::copyMessageTrace(message, identityReply);
        identityReply.mutable_identity()->mutable_nonce()->CopyFrom(message.challenge_request().nonce());
        *identityReply.mutable_identity()->mutable_public_key() = owner_.keyPair().publicKey();
        *identityReply.mutable_identity()->mutable_boot_id() = owner_.bootId();
        *identityReply.mutable_signature() = rsp::signMessage(owner_.keyPair(), identityReply);

        const auto queue = owner_.outputQueue();
        if (queue == nullptr || !queue->push(std::move(identityReply))) {
            std::cerr << "RSPNode failed to enqueue challenge identity reply on the output queue" << std::endl;
        }
        return;
    }

    if (owner_.handleAesNegotiationMessage(message)) {
        return;
    }

    rsp::proto::RSPMessage reply;
    if (message.has_source()) {
        *reply.mutable_destination() = message.source();
    } else {
        const auto senderNodeId = rsp::senderNodeIdFromMessage(message);
        if (senderNodeId.has_value()) {
            *reply.mutable_destination() = toProtoNodeId(*senderNodeId);
        }
    }
    if (message.has_nonce()) {
        *reply.mutable_nonce() = message.nonce();
    }

    if (message.has_ping_request()) {
        auto* pingReply = reply.mutable_ping_reply();
        pingReply->mutable_nonce()->CopyFrom(message.ping_request().nonce());
        pingReply->set_sequence(message.ping_request().sequence());
        pingReply->mutable_time_sent()->CopyFrom(message.ping_request().time_sent());
        *pingReply->mutable_time_replied() = toProtoDateTime(rsp::DateTime());
    } else {
        auto* errorReply = reply.mutable_error();
        errorReply->set_error_code(rsp::proto::UNKNOWN_MESSAGE_TYPE);
        errorReply->set_message("unsupported message for base node handler");
    }

    const auto queue = owner_.outputQueue();
    if (queue == nullptr || !queue->push(std::move(reply))) {
        std::cerr << "RSPNode failed to enqueue reply on the output queue" << std::endl;
    }
}

RSPNode::RSPNode() : RSPNode(KeyPair::generateP256()) {
}

RSPNode::RSPNode(KeyPair keyPair)
    : keyPair_(std::move(keyPair)),
      inputQueue_(std::make_shared<NodeInputQueue>(*this)),
      outputQueue_(std::make_shared<NodeOutputQueue>(*this)),
      identityCache_(keyPair_.nodeID(), [this](rsp::proto::RSPMessage message) {
          return outputQueue_ != nullptr && outputQueue_->push(std::move(message));
      }),
            loggingSubscriptions_(keyPair_.nodeID()) {
    rsp::os::randomFill(instanceSeed_.data(), static_cast<uint32_t>(instanceSeed_.size()));
    std::string bootIdBytes(16, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(bootIdBytes.data()), 16);
    bootId_.set_value(bootIdBytes);
    {
        std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
        aesNegotiationState_.limits.maxActiveKeys = 64;
        aesNegotiationState_.limits.maxPendingNegotiations = 64;
        aesNegotiationState_.limits.defaultLifetimeMs = 5 * 60 * 1000;
        aesNegotiationState_.limits.minLifetimeMs = 1000;
        aesNegotiationState_.limits.maxLifetimeMs = 60 * 60 * 1000;
        aesNegotiationState_.limits.pendingTimeoutMs = 15 * 1000;
        aesNegotiationState_.pendingByPeer.clear();
        aesNegotiationState_.activeByPeer.clear();
    }
    inputQueue_->setWorkerCount(1);
    inputQueue_->start();
    outputQueue_->setWorkerCount(1);
    outputQueue_->start();
}

RSPNode::~RSPNode() {
    stopNodeQueues();
}

void RSPNode::stopNodeQueues() {
    if (inputQueue_ != nullptr) {
        inputQueue_->stop();
    }

    if (outputQueue_ != nullptr) {
        outputQueue_->stop();
    }
}

bool RSPNode::enqueueInput(rsp::proto::RSPMessage message) const {
    processExpiredAesKeys();
    observeMessage(message);
    return inputQueue_ != nullptr && inputQueue_->push(std::move(message));
}

void RSPNode::observeMessage(const rsp::proto::RSPMessage& message) const {
    identityCache_.observeMessage(message);
}

size_t RSPNode::pendingInputCount() const {
    return inputQueue_ == nullptr ? 0 : inputQueue_->size();
}

size_t RSPNode::pendingOutputCount() const {
    return outputQueue_ == nullptr ? 0 : outputQueue_->size();
}

const std::array<uint8_t, 16>& RSPNode::instanceSeed() const {
    return instanceSeed_;
}

const rsp::proto::Uuid& RSPNode::bootId() const {
    return bootId_;
}

const KeyPair& RSPNode::keyPair() const {
    return keyPair_;
}

rsp::MessageQueueHandle RSPNode::inputQueue() const {
    return inputQueue_;
}

rsp::MessageQueueHandle RSPNode::outputQueue() const {
    return outputQueue_;
}

IdentityCache& RSPNode::identityCache() {
    return identityCache_;
}

const IdentityCache& RSPNode::identityCache() const {
    return identityCache_;
}

void RSPNode::setAesNegotiationLimits(const AesNegotiationLimits& limits) {
    AesNegotiationLimits sanitized = limits;
    if (sanitized.minLifetimeMs == 0) {
        sanitized.minLifetimeMs = 1;
    }
    if (sanitized.maxLifetimeMs < sanitized.minLifetimeMs) {
        sanitized.maxLifetimeMs = sanitized.minLifetimeMs;
    }
    sanitized.defaultLifetimeMs = std::clamp(
        sanitized.defaultLifetimeMs, sanitized.minLifetimeMs, sanitized.maxLifetimeMs);
    if (sanitized.pendingTimeoutMs == 0) {
        sanitized.pendingTimeoutMs = 1;
    }

    std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
    aesNegotiationState_.limits = sanitized;
}

RSPNode::AesNegotiationLimits RSPNode::aesNegotiationLimits() const {
    std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
    return aesNegotiationState_.limits;
}

bool RSPNode::beginAesKeyNegotiation(const rsp::NodeID& peerNodeId, uint64_t requestedLifetimeMs) {
    if (peerNodeId == keyPair().nodeID()) {
        return false;
    }

    processExpiredAesKeys();

    rsp::proto::RSPMessage requestMessage;
    try {
        const rsp::DateTime now;
        std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
        auto& state = aesNegotiationState_;

        if (state.pendingByPeer.find(peerNodeId) != state.pendingByPeer.end() ||
            state.activeByPeer.find(peerNodeId) != state.activeByPeer.end()) {
            return false;
        }

        if (state.limits.maxPendingNegotiations == 0 ||
            state.pendingByPeer.size() >= state.limits.maxPendingNegotiations) {
            return false;
        }

        if (state.limits.maxActiveKeys == 0 ||
            state.activeByPeer.size() >= state.limits.maxActiveKeys) {
            return false;
        }

        const uint64_t lifetimeMs = clampLifetimeMs(requestedLifetimeMs, state.limits);
        const std::string keyId = randomKeyId();
        KeyPair ephemeralKeyPair = KeyPair::generateP256();

        *requestMessage.mutable_source() = toProtoNodeId(keyPair().nodeID());
        *requestMessage.mutable_destination() = toProtoNodeId(peerNodeId);
        auto* request = requestMessage.mutable_aes_key_negotiation_request();
        request->mutable_key_id()->set_value(keyId);
        *request->mutable_ephemeral_public_key() = ephemeralKeyPair.publicKey();
        request->set_requested_lifetime_ms(lifetimeMs);
        request->set_algorithm(rsp::proto::KEY_NEGOTIATION_ALGORITHM_P256_SHA256_AES256);

        PendingAesNegotiation pending;
        pending.keyId = keyId;
        pending.ephemeralKeyPair = std::move(ephemeralKeyPair);
        pending.algorithm = static_cast<uint32_t>(request->algorithm());
        pending.requestedLifetimeMs = lifetimeMs;
        pending.expiresAt = addMilliseconds(now, state.limits.pendingTimeoutMs);
        state.pendingByPeer.emplace(peerNodeId, std::move(pending));
    } catch (const std::exception&) {
        return false;
    }

    if (!pushOutputMessage(std::move(requestMessage), "AES key negotiation request")) {
        std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
        aesNegotiationState_.pendingByPeer.erase(peerNodeId);
        return false;
    }
    return true;
}

bool RSPNode::dropAesKey(const rsp::NodeID& peerNodeId, const std::string& reason) {
    processExpiredAesKeys();

    std::optional<rsp::proto::RSPMessage> dropNotification;
    {
        std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
        aesNegotiationState_.pendingByPeer.erase(peerNodeId);

        const auto keyIt = aesNegotiationState_.activeByPeer.find(peerNodeId);
        if (keyIt == aesNegotiationState_.activeByPeer.end()) {
            return false;
        }

        dropNotification = makeAesKeyDropNotification(
            keyPair().nodeID(),
            peerNodeId,
            keyIt->second.keyId,
            reason.empty() ? "local_drop" : reason);
        aesNegotiationState_.activeByPeer.erase(keyIt);
    }

    return dropNotification.has_value() &&
           pushOutputMessage(std::move(*dropNotification), "AES key drop notification");
}

bool RSPNode::hasAesKey(const rsp::NodeID& peerNodeId) const {
    processExpiredAesKeys();
    std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
    return aesNegotiationState_.activeByPeer.find(peerNodeId) != aesNegotiationState_.activeByPeer.end();
}

size_t RSPNode::activeAesKeyCount() const {
    processExpiredAesKeys();
    std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
    return aesNegotiationState_.activeByPeer.size();
}

size_t RSPNode::pendingAesNegotiationCount() const {
    processExpiredAesKeys();
    std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
    return aesNegotiationState_.pendingByPeer.size();
}

std::optional<RSPNode::AesNegotiatedKeyInfo> RSPNode::aesKeyInfo(const rsp::NodeID& peerNodeId) const {
    processExpiredAesKeys();

    std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
    const auto keyIt = aesNegotiationState_.activeByPeer.find(peerNodeId);
    if (keyIt == aesNegotiationState_.activeByPeer.end() || !isValidKeyId(keyIt->second.keyId)) {
        return std::nullopt;
    }

    const std::string& keyIdBytes = keyIt->second.keyId;
    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, keyIdBytes.data(), sizeof(high));
    std::memcpy(&low, keyIdBytes.data() + sizeof(high), sizeof(low));

    return AesNegotiatedKeyInfo{
        rsp::GUID(high, low),
        peerNodeId,
        keyIt->second.algorithm,
        keyIt->second.expiresAt,
    };
}

std::optional<rsp::Buffer> RSPNode::aesKeyMaterialForPeer(const rsp::NodeID& peerNodeId) const {
    processExpiredAesKeys();
    std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
    const auto keyIt = aesNegotiationState_.activeByPeer.find(peerNodeId);
    if (keyIt == aesNegotiationState_.activeByPeer.end()) {
        return std::nullopt;
    }
    return keyIt->second.keyMaterial;
}

bool RSPNode::pushOutputMessage(rsp::proto::RSPMessage message, const char* context) const {
    const auto queue = outputQueue();
    if (queue == nullptr || !queue->push(std::move(message))) {
        std::cerr << "RSPNode failed to enqueue "
                  << (context == nullptr ? "message" : context)
                  << " on the output queue" << std::endl;
        return false;
    }
    return true;
}

void RSPNode::processExpiredAesKeys() const {
    const rsp::DateTime now;
    std::vector<rsp::proto::RSPMessage> dropNotifications;

    {
        std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
        for (auto it = aesNegotiationState_.activeByPeer.begin();
             it != aesNegotiationState_.activeByPeer.end();) {
            if (it->second.expiresAt > now) {
                ++it;
                continue;
            }

            dropNotifications.push_back(makeAesKeyDropNotification(
                keyPair().nodeID(), it->first, it->second.keyId, "expired"));
            it = aesNegotiationState_.activeByPeer.erase(it);
        }

        for (auto it = aesNegotiationState_.pendingByPeer.begin();
             it != aesNegotiationState_.pendingByPeer.end();) {
            if (it->second.expiresAt > now) {
                ++it;
                continue;
            }
            it = aesNegotiationState_.pendingByPeer.erase(it);
        }
    }

    for (auto& drop : dropNotifications) {
        pushOutputMessage(std::move(drop), "expired AES key drop notification");
    }
}

void RSPNode::handleAesKeyNegotiationError(const rsp::proto::RSPMessage& message) {
    if (!message.has_aes_key_negotiation_error()) {
        return;
    }

    const auto senderNodeId = rsp::senderNodeIdFromMessage(message);
    if (!senderNodeId.has_value()) {
        return;
    }

    const auto& error = message.aes_key_negotiation_error();
    if (!error.has_key_id() || !isValidKeyId(error.key_id().value())) {
        return;
    }

    const std::string keyId = error.key_id().value();

    std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
    const auto pendingIt = aesNegotiationState_.pendingByPeer.find(*senderNodeId);
    if (pendingIt != aesNegotiationState_.pendingByPeer.end() &&
        pendingIt->second.keyId == keyId) {
        aesNegotiationState_.pendingByPeer.erase(pendingIt);
    }

    if (error.error_code() == rsp::proto::KEY_NEGOTIATION_ERROR_KEY_INCORRECT ||
        error.error_code() == rsp::proto::KEY_NEGOTIATION_ERROR_UNKNOWN_KEY ||
        error.error_code() == rsp::proto::KEY_NEGOTIATION_ERROR_EXPIRED) {
        const auto keyIt = aesNegotiationState_.activeByPeer.find(*senderNodeId);
        if (keyIt != aesNegotiationState_.activeByPeer.end() && keyIt->second.keyId == keyId) {
            aesNegotiationState_.activeByPeer.erase(keyIt);
        }
    }
}

bool RSPNode::handleAesNegotiationMessage(const rsp::proto::RSPMessage& message) {
    const bool isAesNegotiationMessage = message.has_aes_key_negotiation_request() ||
                                         message.has_aes_key_negotiation_reply() ||
                                         message.has_aes_key_drop_notification() ||
                                         message.has_aes_key_negotiation_error();
    if (!isAesNegotiationMessage) {
        return false;
    }

    processExpiredAesKeys();

    if (message.has_aes_key_negotiation_error()) {
        handleAesKeyNegotiationError(message);
        return true;
    }

    const auto senderNodeId = rsp::senderNodeIdFromMessage(message);
    if (!senderNodeId.has_value()) {
        return true;
    }

    const rsp::NodeID localNodeId = keyPair().nodeID();
    std::optional<rsp::proto::RSPMessage> outboundReply;

    if (message.has_aes_key_negotiation_request()) {
        const auto& request = message.aes_key_negotiation_request();
        const std::optional<std::string> keyId =
            request.has_key_id() ? std::optional<std::string>(request.key_id().value()) : std::nullopt;

        if (!keyId.has_value() || !isValidKeyId(*keyId) || !request.has_ephemeral_public_key()) {
            outboundReply = makeAesKeyNegotiationError(
                localNodeId,
                *senderNodeId,
                rsp::proto::KEY_NEGOTIATION_ERROR_INVALID_REQUEST,
                keyId,
                "invalid AES key negotiation request");
        } else if (request.algorithm() != rsp::proto::KEY_NEGOTIATION_ALGORITHM_P256_SHA256_AES256) {
            outboundReply = makeAesKeyNegotiationError(
                localNodeId,
                *senderNodeId,
                rsp::proto::KEY_NEGOTIATION_ERROR_UNSUPPORTED_ALGORITHM,
                *keyId,
                "unsupported AES key negotiation algorithm");
        } else {
            const rsp::DateTime now;
            std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
            auto& state = aesNegotiationState_;

            if (state.pendingByPeer.find(*senderNodeId) != state.pendingByPeer.end()) {
                outboundReply = makeAesKeyNegotiationError(
                    localNodeId,
                    *senderNodeId,
                    rsp::proto::KEY_NEGOTIATION_ERROR_PAIR_NEGOTIATION_EXISTS,
                    *keyId,
                    "peer already has a pending AES negotiation");
            } else if (state.activeByPeer.find(*senderNodeId) != state.activeByPeer.end()) {
                outboundReply = makeAesKeyNegotiationError(
                    localNodeId,
                    *senderNodeId,
                    rsp::proto::KEY_NEGOTIATION_ERROR_PAIR_KEY_EXISTS,
                    *keyId,
                    "peer already has an active AES key");
            } else if (state.limits.maxActiveKeys == 0 ||
                       state.activeByPeer.size() >= state.limits.maxActiveKeys) {
                outboundReply = makeAesKeyNegotiationError(
                    localNodeId,
                    *senderNodeId,
                    rsp::proto::KEY_NEGOTIATION_ERROR_KEY_LIMIT_REACHED,
                    *keyId,
                    "AES key capacity reached");
            } else {
                try {
                    const KeyPair peerEphemeralKey = KeyPair::fromPublicKey(request.ephemeral_public_key());
                    KeyPair responderEphemeralKey = KeyPair::generateP256();
                    const auto sharedSecret = deriveSharedSecret(
                        responderEphemeralKey.get(), peerEphemeralKey.get());
                    if (!sharedSecret.has_value()) {
                        outboundReply = makeAesKeyNegotiationError(
                            localNodeId,
                            *senderNodeId,
                            rsp::proto::KEY_NEGOTIATION_ERROR_KEY_INCORRECT,
                            *keyId,
                            "failed to derive shared AES key material");
                    } else {
                        const uint64_t acceptedLifetimeMs = clampLifetimeMs(
                            request.requested_lifetime_ms(), state.limits);
                        ActiveAesKey active;
                        active.keyId = *keyId;
                        active.keyMaterial = deriveAesKeyMaterial(
                            *sharedSecret, *keyId, localNodeId, *senderNodeId);
                        active.algorithm = static_cast<uint32_t>(request.algorithm());
                        active.expiresAt = addMilliseconds(now, acceptedLifetimeMs);
                        state.activeByPeer[*senderNodeId] = std::move(active);

                        rsp::proto::RSPMessage reply;
                        *reply.mutable_source() = toProtoNodeId(localNodeId);
                        *reply.mutable_destination() = toProtoNodeId(*senderNodeId);
                        auto* keyReply = reply.mutable_aes_key_negotiation_reply();
                        keyReply->mutable_key_id()->set_value(*keyId);
                        *keyReply->mutable_ephemeral_public_key() = responderEphemeralKey.publicKey();
                        keyReply->set_accepted_lifetime_ms(acceptedLifetimeMs);
                        keyReply->set_algorithm(request.algorithm());
                        outboundReply = std::move(reply);
                    }
                } catch (const std::exception&) {
                    outboundReply = makeAesKeyNegotiationError(
                        localNodeId,
                        *senderNodeId,
                        rsp::proto::KEY_NEGOTIATION_ERROR_INVALID_REQUEST,
                        *keyId,
                        "invalid AES key negotiation public key");
                }
            }
        }
    } else if (message.has_aes_key_negotiation_reply()) {
        const auto& reply = message.aes_key_negotiation_reply();
        const std::optional<std::string> keyId =
            reply.has_key_id() ? std::optional<std::string>(reply.key_id().value()) : std::nullopt;

        if (!keyId.has_value() || !isValidKeyId(*keyId) || !reply.has_ephemeral_public_key()) {
            outboundReply = makeAesKeyNegotiationError(
                localNodeId,
                *senderNodeId,
                rsp::proto::KEY_NEGOTIATION_ERROR_INVALID_REQUEST,
                keyId,
                "invalid AES key negotiation reply");
        } else if (reply.algorithm() != rsp::proto::KEY_NEGOTIATION_ALGORITHM_P256_SHA256_AES256) {
            outboundReply = makeAesKeyNegotiationError(
                localNodeId,
                *senderNodeId,
                rsp::proto::KEY_NEGOTIATION_ERROR_UNSUPPORTED_ALGORITHM,
                *keyId,
                "unsupported AES key negotiation algorithm");
        } else {
            const rsp::DateTime now;
            std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
            auto& state = aesNegotiationState_;

            auto pendingIt = state.pendingByPeer.find(*senderNodeId);
            if (pendingIt == state.pendingByPeer.end()) {
                outboundReply = makeAesKeyNegotiationError(
                    localNodeId,
                    *senderNodeId,
                    rsp::proto::KEY_NEGOTIATION_ERROR_UNKNOWN_KEY,
                    *keyId,
                    "no pending AES negotiation for peer");
            } else if (pendingIt->second.keyId != *keyId) {
                outboundReply = makeAesKeyNegotiationError(
                    localNodeId,
                    *senderNodeId,
                    rsp::proto::KEY_NEGOTIATION_ERROR_KEY_INCORRECT,
                    *keyId,
                    "reply key id does not match the pending negotiation");
            } else if (state.limits.maxActiveKeys == 0 ||
                       (state.activeByPeer.find(*senderNodeId) == state.activeByPeer.end() &&
                        state.activeByPeer.size() >= state.limits.maxActiveKeys)) {
                state.pendingByPeer.erase(pendingIt);
                outboundReply = makeAesKeyNegotiationError(
                    localNodeId,
                    *senderNodeId,
                    rsp::proto::KEY_NEGOTIATION_ERROR_KEY_LIMIT_REACHED,
                    *keyId,
                    "AES key capacity reached");
            } else {
                try {
                    const KeyPair peerEphemeralKey = KeyPair::fromPublicKey(reply.ephemeral_public_key());
                    const auto sharedSecret = deriveSharedSecret(
                        pendingIt->second.ephemeralKeyPair.get(), peerEphemeralKey.get());
                    if (!sharedSecret.has_value()) {
                        state.pendingByPeer.erase(pendingIt);
                        outboundReply = makeAesKeyNegotiationError(
                            localNodeId,
                            *senderNodeId,
                            rsp::proto::KEY_NEGOTIATION_ERROR_KEY_INCORRECT,
                            *keyId,
                            "failed to derive shared AES key material");
                    } else {
                        const uint64_t requestedLifetime = pendingIt->second.requestedLifetimeMs;
                        const uint64_t acceptedLifetimeMs = clampLifetimeMs(
                            reply.accepted_lifetime_ms() == 0 ? requestedLifetime : reply.accepted_lifetime_ms(),
                            state.limits);

                        ActiveAesKey active;
                        active.keyId = pendingIt->second.keyId;
                        active.keyMaterial = deriveAesKeyMaterial(
                            *sharedSecret, *keyId, localNodeId, *senderNodeId);
                        active.algorithm = static_cast<uint32_t>(reply.algorithm());
                        active.expiresAt = addMilliseconds(now, acceptedLifetimeMs);
                        state.activeByPeer[*senderNodeId] = std::move(active);
                        state.pendingByPeer.erase(pendingIt);
                    }
                } catch (const std::exception&) {
                    outboundReply = makeAesKeyNegotiationError(
                        localNodeId,
                        *senderNodeId,
                        rsp::proto::KEY_NEGOTIATION_ERROR_INVALID_REQUEST,
                        *keyId,
                        "invalid AES key negotiation public key");
                }
            }
        }
    } else if (message.has_aes_key_drop_notification()) {
        const auto& drop = message.aes_key_drop_notification();
        const std::optional<std::string> keyId =
            drop.has_key_id() ? std::optional<std::string>(drop.key_id().value()) : std::nullopt;

        if (!keyId.has_value() || !isValidKeyId(*keyId)) {
            outboundReply = makeAesKeyNegotiationError(
                localNodeId,
                *senderNodeId,
                rsp::proto::KEY_NEGOTIATION_ERROR_INVALID_REQUEST,
                keyId,
                "invalid AES key drop notification");
        } else {
            std::lock_guard<std::mutex> lock(aesNegotiationMutex_);
            auto& state = aesNegotiationState_;
            auto activeIt = state.activeByPeer.find(*senderNodeId);
            if (activeIt == state.activeByPeer.end()) {
                outboundReply = makeAesKeyNegotiationError(
                    localNodeId,
                    *senderNodeId,
                    rsp::proto::KEY_NEGOTIATION_ERROR_UNKNOWN_KEY,
                    *keyId,
                    "AES key not found for peer");
            } else if (activeIt->second.keyId != *keyId) {
                outboundReply = makeAesKeyNegotiationError(
                    localNodeId,
                    *senderNodeId,
                    rsp::proto::KEY_NEGOTIATION_ERROR_KEY_INCORRECT,
                    *keyId,
                    "AES key id mismatch for peer");
            } else {
                state.activeByPeer.erase(activeIt);
            }

            const auto pendingIt = state.pendingByPeer.find(*senderNodeId);
            if (pendingIt != state.pendingByPeer.end() && pendingIt->second.keyId == *keyId) {
                state.pendingByPeer.erase(pendingIt);
            }
        }
    }

    if (outboundReply.has_value()) {
        pushOutputMessage(std::move(*outboundReply), "AES key negotiation response");
    }
    return true;
}

bool RSPNode::publishLogRecord(const rsp::proto::LogRecord& record,
                               const rsp::resource_manager::SchemaSnapshot* schemaSnapshot) {
    if (loggingSubscriptions_.subscriptionCount() == 0) {
        return true;
    }

    const auto queue = outputQueue();
    if (queue == nullptr) {
        return false;
    }

    loggingSubscriptions_.publish(
        record,
        [queue](const rsp::proto::RSPMessage& message) {
            return queue->push(message);
        },
        schemaSnapshot,
        rsp::DateTime());
    return true;
}

}  // namespace rsp
