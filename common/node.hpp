#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>

#include "common/logging/logging.hpp"
#include "common/keypair.hpp"
#include "common/message_queue/mq.hpp"
#include "messages.pb.h"

namespace rsp {

class NodeInputQueue;

class IdentityCache {
public:
    using SendMessageCallback = std::function<bool(rsp::proto::RSPMessage)>;

    IdentityCache() = default;
    IdentityCache(NodeID localNodeId, SendMessageCallback sendMessageCallback, size_t maximumEntries = 1024);

    bool observeMessage(const rsp::proto::RSPMessage& message);
    bool sendChallengeRequest(const rsp::NodeID& nodeId) const;

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
    std::list<rsp::NodeID> usageOrder_;
    std::map<rsp::NodeID, CacheEntry> entries_;
};

class RSPNode {
public:
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

private:
    friend class NodeInputQueue;
    friend class NodeOutputQueue;

    KeyPair keyPair_;
    std::array<uint8_t, 16> instanceSeed_;
    rsp::proto::Uuid bootId_;
    rsp::MessageQueueHandle inputQueue_;
    rsp::MessageQueueHandle outputQueue_;
    mutable IdentityCache identityCache_;
    rsp::logging::SubscriptionManager loggingSubscriptions_;
};

}  // namespace rsp