#pragma once

#include "common/base_types.hpp"
#include "logging/logging.pb.h"
#include "messages.pb.h"
#include "resource_manager/schema_registry.hpp"

#include <google/protobuf/message.h>

#include <cstddef>
#include <functional>
#include <optional>

namespace rsp::logging {

struct PublishStats {
    std::size_t matched_subscriptions = 0;
    std::size_t delivered_messages = 0;
    std::size_t expired_subscriptions = 0;
    std::size_t removed_subscriptions_on_failure = 0;
};

std::optional<rsp::GUID> guidFromProto(const rsp::proto::Uuid& uuid);
std::optional<rsp::NodeID> nodeIdFromProto(const rsp::proto::NodeId& nodeId);
rsp::proto::Uuid guidToProto(const rsp::GUID& guid);
rsp::proto::NodeId nodeIdToProto(const rsp::NodeID& nodeId);
rsp::proto::DateTime dateTimeToProto(const rsp::DateTime& dateTime);

bool evaluateFilterTree(const rsp::proto::LogASTMessageTree& tree,
                        const google::protobuf::Message& message,
                        const rsp::resource_manager::SchemaSnapshot* schemaSnapshot = nullptr);

class SubscriptionManager {
public:
    using DeliverCallback = std::function<bool(const rsp::proto::RSPMessage&)>;

    explicit SubscriptionManager(rsp::NodeID localNodeId);

    rsp::proto::LogSubscribeReply subscribe(const rsp::NodeID& subscriberNodeId,
                                            const rsp::proto::LogSubscribeRequest& request,
                                            const rsp::DateTime& now = rsp::DateTime());
    rsp::proto::LogUnsubscribeReply unsubscribe(const rsp::NodeID& subscriberNodeId,
                                                const rsp::proto::LogUnsubscribeRequest& request);
    PublishStats publish(const rsp::proto::LogRecord& record,
                         const DeliverCallback& deliver,
                         const rsp::resource_manager::SchemaSnapshot* schemaSnapshot = nullptr,
                         const rsp::DateTime& now = rsp::DateTime());
    std::size_t removeExpired(const rsp::DateTime& now = rsp::DateTime());
    std::size_t removeAllForNode(const rsp::NodeID& subscriberNodeId);
    std::size_t subscriptionCount() const;
    std::size_t subscriptionCountForNode(const rsp::NodeID& subscriberNodeId) const;
    bool hasSubscription(const rsp::GUID& subscriptionId) const;

private:
    struct Subscription {
        rsp::NodeID subscriberNodeId;
        std::string payloadTypeUrl;
        rsp::proto::LogASTMessageTree filter;
        rsp::DateTime expiresAt;
    };

    rsp::NodeID localNodeId_;
    mutable std::mutex mutex_;
    std::map<rsp::GUID, Subscription> subscriptions_;
};

}  // namespace rsp::logging