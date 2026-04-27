#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "common/logging/logging.hpp"
#include "common/keypair.hpp"
#include "common/message_queue/mq.hpp"
#include "messages.pb.h"

namespace rsp {

class NodeInputQueue;

class IdentityCache {
public:
    using SendMessageCallback = std::function<bool(rsp::proto::RSPMessage)>;
    using IdentityObservedCallback = std::function<void(const rsp::NodeID&)>;
    using IdentityObservedCallbackToken = uint64_t;

    IdentityCache() = default;
    IdentityCache(NodeID localNodeId, SendMessageCallback sendMessageCallback, size_t maximumEntries = 1024);

    bool observeMessage(const rsp::proto::RSPMessage& message);
    bool sendChallengeRequest(const rsp::NodeID& nodeId) const;
    IdentityObservedCallbackToken addIdentityObservedCallback(IdentityObservedCallback callback);
    bool removeIdentityObservedCallback(IdentityObservedCallbackToken token);

    std::optional<rsp::proto::Identity> get(const rsp::NodeID& nodeId) const;
    bool contains(const rsp::NodeID& nodeId) const;
    size_t size() const;
    size_t maximumEntries() const;
    void setMaximumEntries(size_t maximumEntries);

private:
    struct CacheEntry {
        rsp::proto::Identity identity;
        std::list<rsp::NodeID>::iterator usage;
    };

    bool cacheIdentity(const rsp::proto::Identity& identity,
                       const std::optional<rsp::NodeID>& sourceNodeId = std::nullopt);
    void trimToLimitLocked();

    mutable std::mutex mutex_;
    size_t maximumEntries_ = 1024;
    std::optional<rsp::NodeID> localNodeId_;
    SendMessageCallback sendMessageCallback_;
    IdentityObservedCallbackToken nextIdentityObservedCallbackToken_ = 1;
    std::map<IdentityObservedCallbackToken, IdentityObservedCallback> identityObservedCallbacks_;
    std::list<rsp::NodeID> usageOrder_;
    std::map<rsp::NodeID, CacheEntry> entries_;
};

class RSPNode {
public:
    struct AesNegotiationLimits {
        size_t maxActiveKeys = 64;
        size_t maxPendingNegotiations = 64;
        uint64_t defaultLifetimeMs = 5 * 60 * 1000;
        uint64_t minLifetimeMs = 1000;
        uint64_t maxLifetimeMs = 60 * 60 * 1000;
        uint64_t pendingTimeoutMs = 15 * 1000;
    };

    struct AesNegotiatedKeyInfo {
        rsp::GUID keyId;
        rsp::NodeID peerNodeId;
        uint32_t algorithm = 0;
        rsp::DateTime expiresAt;
    };

    RSPNode();
    explicit RSPNode(KeyPair keyPair);
    virtual ~RSPNode();

    virtual int run() const = 0;

    bool enqueueInput(rsp::proto::RSPMessage message) const;
    void observeMessage(const rsp::proto::RSPMessage& message) const;
    size_t pendingInputCount() const;
    size_t pendingOutputCount() const;
    IdentityCache& identityCache();
    const IdentityCache& identityCache() const;
    void setAesNegotiationLimits(const AesNegotiationLimits& limits);
    AesNegotiationLimits aesNegotiationLimits() const;
    bool beginAesKeyNegotiation(const rsp::NodeID& peerNodeId, uint64_t requestedLifetimeMs = 0);
    bool dropAesKey(const rsp::NodeID& peerNodeId, const std::string& reason = "local_drop");
    bool hasAesKey(const rsp::NodeID& peerNodeId) const;
    size_t activeAesKeyCount() const;
    size_t pendingAesNegotiationCount() const;
    std::optional<AesNegotiatedKeyInfo> aesKeyInfo(const rsp::NodeID& peerNodeId) const;

protected:
    virtual bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) = 0;
    virtual void handleOutputMessage(rsp::proto::RSPMessage message) = 0;
    void stopNodeQueues();
    bool publishLogRecord(const rsp::proto::LogRecord& record,
                          const rsp::resource_manager::SchemaSnapshot* schemaSnapshot);

    const std::array<uint8_t, 16>& instanceSeed() const;
    const rsp::proto::Uuid& bootId() const;
    const KeyPair& keyPair() const;
    rsp::MessageQueueHandle inputQueue() const;
    rsp::MessageQueueHandle outputQueue() const;
    std::optional<rsp::Buffer> aesKeyMaterialForPeer(const rsp::NodeID& peerNodeId) const;

private:
    friend class NodeInputQueue;
    friend class NodeOutputQueue;

    struct PendingAesNegotiation {
        std::string keyId;
        KeyPair ephemeralKeyPair;
        uint32_t algorithm = 0;
        uint64_t requestedLifetimeMs = 0;
        rsp::DateTime expiresAt;
    };

    struct ActiveAesKey {
        std::string keyId;
        rsp::Buffer keyMaterial;
        uint32_t algorithm = 0;
        rsp::DateTime expiresAt;
    };

    struct AesNegotiationState {
        AesNegotiationLimits limits;
        std::map<rsp::NodeID, PendingAesNegotiation> pendingByPeer;
        std::map<rsp::NodeID, ActiveAesKey> activeByPeer;
    };

    bool handleAesNegotiationMessage(const rsp::proto::RSPMessage& message);
    void handleAesKeyNegotiationError(const rsp::proto::RSPMessage& message);
    bool pushOutputMessage(rsp::proto::RSPMessage message, const char* context) const;
    void processExpiredAesKeys() const;

    KeyPair keyPair_;
    std::array<uint8_t, 16> instanceSeed_;
    rsp::proto::Uuid bootId_;
    rsp::MessageQueueHandle inputQueue_;
    rsp::MessageQueueHandle outputQueue_;
    mutable IdentityCache identityCache_;
    mutable std::mutex aesNegotiationMutex_;
    mutable AesNegotiationState aesNegotiationState_;
    rsp::logging::SubscriptionManager loggingSubscriptions_;
};

}  // namespace rsp
