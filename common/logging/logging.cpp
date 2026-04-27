#include "common/logging/logging.hpp"

#include "common/endorsement/field_resolver.hpp"

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace rsp::logging {

namespace {

struct SubscriptionSnapshot {
    rsp::GUID id;
    rsp::NodeID subscriberNodeId;
    std::string payloadTypeUrl;
    rsp::proto::LogASTMessageTree filter;
    rsp::DateTime expiresAt;
};

bool hasRequiredUuidBytes(const std::string& value) {
    return value.size() == 16;
}

template <typename GuidLike>
GuidLike parseGuidLike(const std::string& value) {
    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, value.data(), sizeof(high));
    std::memcpy(&low, value.data() + sizeof(high), sizeof(low));
    return GuidLike(high, low);
}

template <typename GuidLike>
std::string encodeGuidLike(const GuidLike& guid) {
    std::string value(16, '\0');
    const uint64_t high = guid.high();
    const uint64_t low = guid.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    return value;
}

std::string typeNameFromUrl(const std::string& typeUrl) {
    const auto pos = typeUrl.rfind('/');
    if (pos == std::string::npos) {
        return typeUrl;
    }

    return typeUrl.substr(pos + 1);
}

rsp::proto::ERDASTFieldPath toFieldPath(const rsp::proto::LogASTFieldPath& path) {
    rsp::proto::ERDASTFieldPath converted;
    for (int index = 0; index < path.segments_size(); ++index) {
        converted.add_segments(path.segments(index));
    }
    return converted;
}

rsp::proto::ERDASTFieldValue toFieldValue(const rsp::proto::LogASTFieldValue& value) {
    rsp::proto::ERDASTFieldValue converted;
    switch (value.value_case()) {
    case rsp::proto::LogASTFieldValue::kBytesValue:
        converted.set_bytes_value(value.bytes_value());
        break;
    case rsp::proto::LogASTFieldValue::kStringValue:
        converted.set_string_value(value.string_value());
        break;
    case rsp::proto::LogASTFieldValue::kIntValue:
        converted.set_int_value(value.int_value());
        break;
    case rsp::proto::LogASTFieldValue::kUintValue:
        converted.set_uint_value(value.uint_value());
        break;
    case rsp::proto::LogASTFieldValue::kBoolValue:
        converted.set_bool_value(value.bool_value());
        break;
    case rsp::proto::LogASTFieldValue::kEnumValue:
        converted.set_enum_value(value.enum_value());
        break;
    case rsp::proto::LogASTFieldValue::VALUE_NOT_SET:
        break;
    }
    return converted;
}

bool evaluateFilterTreeImpl(const rsp::proto::LogASTMessageTree& tree,
                            const google::protobuf::Message& message,
                            const rsp::resource_manager::SchemaSnapshot* schemaSnapshot) {
    switch (tree.node_type_case()) {
    case rsp::proto::LogASTMessageTree::kEquals:
        return tree.equals().has_lhs() && tree.equals().has_rhs() &&
               evaluateFilterTreeImpl(tree.equals().lhs(), message, schemaSnapshot) ==
                   evaluateFilterTreeImpl(tree.equals().rhs(), message, schemaSnapshot);
    case rsp::proto::LogASTMessageTree::kAnd:
        return tree.and_().has_lhs() && tree.and_().has_rhs() &&
               evaluateFilterTreeImpl(tree.and_().lhs(), message, schemaSnapshot) &&
               evaluateFilterTreeImpl(tree.and_().rhs(), message, schemaSnapshot);
    case rsp::proto::LogASTMessageTree::kOr:
        return tree.or_().has_lhs() && tree.or_().has_rhs() &&
               (evaluateFilterTreeImpl(tree.or_().lhs(), message, schemaSnapshot) ||
                evaluateFilterTreeImpl(tree.or_().rhs(), message, schemaSnapshot));
    case rsp::proto::LogASTMessageTree::kAllOf:
        if (tree.all_of().terms_size() == 0) {
            return false;
        }

        for (const auto& term : tree.all_of().terms()) {
            if (!evaluateFilterTreeImpl(term, message, schemaSnapshot)) {
                return false;
            }
        }

        return true;
    case rsp::proto::LogASTMessageTree::kAnyOf:
        for (const auto& term : tree.any_of().terms()) {
            if (evaluateFilterTreeImpl(term, message, schemaSnapshot)) {
                return true;
            }
        }

        return false;
    case rsp::proto::LogASTMessageTree::kFieldEquals: {
        if (!tree.field_equals().has_path() || !tree.field_equals().has_value()) {
            return false;
        }
        const auto resolved = rsp::endorsement::resolveFieldPath(
            toFieldPath(tree.field_equals().path()), message, schemaSnapshot);
        return rsp::endorsement::resolvedValueEquals(resolved, toFieldValue(tree.field_equals().value()));
    }
    case rsp::proto::LogASTMessageTree::kFieldExists: {
        if (!tree.field_exists().has_path()) {
            return false;
        }
        const auto resolved = rsp::endorsement::resolveFieldPath(
            toFieldPath(tree.field_exists().path()), message, schemaSnapshot);
        return rsp::endorsement::resolvedValuePresent(resolved);
    }
    case rsp::proto::LogASTMessageTree::kFieldContains: {
        if (!tree.field_contains().has_path() || !tree.field_contains().has_sub_path() ||
            !tree.field_contains().has_value()) {
            return false;
        }

        const auto elements = rsp::endorsement::resolveRepeatedMessages(
            toFieldPath(tree.field_contains().path()), message, schemaSnapshot);
        for (const auto* element : elements) {
            const auto resolved = rsp::endorsement::resolveFieldPath(
                toFieldPath(tree.field_contains().sub_path()), *element, schemaSnapshot);
            if (rsp::endorsement::resolvedValueEquals(resolved, toFieldValue(tree.field_contains().value()))) {
                return true;
            }
        }

        return false;
    }
    case rsp::proto::LogASTMessageTree::kTrueValue:
        return true;
    case rsp::proto::LogASTMessageTree::kFalseValue:
        return false;
    case rsp::proto::LogASTMessageTree::NODE_TYPE_NOT_SET:
        return false;
    }

    return false;
}

}  // namespace

std::optional<rsp::GUID> guidFromProto(const rsp::proto::Uuid& uuid) {
    if (!hasRequiredUuidBytes(uuid.value())) {
        return std::nullopt;
    }
    return parseGuidLike<rsp::GUID>(uuid.value());
}

std::optional<rsp::NodeID> nodeIdFromProto(const rsp::proto::NodeId& nodeId) {
    if (!hasRequiredUuidBytes(nodeId.value())) {
        return std::nullopt;
    }
    return parseGuidLike<rsp::NodeID>(nodeId.value());
}

rsp::proto::Uuid guidToProto(const rsp::GUID& guid) {
    rsp::proto::Uuid uuid;
    uuid.set_value(encodeGuidLike(guid));
    return uuid;
}

rsp::proto::NodeId nodeIdToProto(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    protoNodeId.set_value(encodeGuidLike(nodeId));
    return protoNodeId;
}

rsp::proto::DateTime dateTimeToProto(const rsp::DateTime& dateTime) {
    rsp::proto::DateTime protoDateTime;
    protoDateTime.set_milliseconds_since_epoch(dateTime.millisecondsSinceEpoch());
    return protoDateTime;
}

bool evaluateFilterTree(const rsp::proto::LogASTMessageTree& tree,
                        const google::protobuf::Message& message,
                        const rsp::resource_manager::SchemaSnapshot* schemaSnapshot) {
    return evaluateFilterTreeImpl(tree, message, schemaSnapshot);
}

SubscriptionManager::SubscriptionManager(rsp::NodeID localNodeId)
    : localNodeId_(std::move(localNodeId)) {
}

rsp::proto::LogSubscribeReply SubscriptionManager::subscribe(const rsp::NodeID& subscriberNodeId,
                                                             const rsp::proto::LogSubscribeRequest& request,
                                                             const rsp::DateTime& now) {
    rsp::proto::LogSubscribeReply reply;
    if (request.payload_type_url().empty() || !request.has_filter() || request.duration_ms() == 0) {
        reply.set_status(rsp::proto::LOG_SUBSCRIPTION_STATUS_INVALID);
        reply.set_message("log subscriptions require payload_type_url, filter, and non-zero duration_ms");
        return reply;
    }

    const rsp::GUID subscriptionId;
    rsp::DateTime expiresAt = now;
    expiresAt += static_cast<double>(request.duration_ms()) / 1000.0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_.emplace(
            subscriptionId,
            Subscription{subscriberNodeId, request.payload_type_url(), request.filter(), expiresAt});
    }

    reply.set_status(rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED);
    *reply.mutable_subscription_id() = guidToProto(subscriptionId);
    *reply.mutable_expires_at() = dateTimeToProto(expiresAt);
    return reply;
}

rsp::proto::LogUnsubscribeReply SubscriptionManager::unsubscribe(const rsp::NodeID& subscriberNodeId,
                                                                 const rsp::proto::LogUnsubscribeRequest& request) {
    rsp::proto::LogUnsubscribeReply reply;
    if (!request.has_subscription_id()) {
        reply.set_status(rsp::proto::LOG_SUBSCRIPTION_STATUS_INVALID);
        reply.set_message("subscription_id is required");
        return reply;
    }

    const auto subscriptionId = guidFromProto(request.subscription_id());
    if (!subscriptionId.has_value()) {
        reply.set_status(rsp::proto::LOG_SUBSCRIPTION_STATUS_INVALID);
        reply.set_message("subscription_id must be 16 bytes");
        return reply;
    }

    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = subscriptions_.find(*subscriptionId);
        if (it != subscriptions_.end() && it->second.subscriberNodeId == subscriberNodeId) {
            subscriptions_.erase(it);
            removed = true;
        }
    }

    reply.set_status(removed ? rsp::proto::LOG_SUBSCRIPTION_STATUS_REMOVED
                             : rsp::proto::LOG_SUBSCRIPTION_STATUS_NOT_FOUND);
    *reply.mutable_subscription_id() = request.subscription_id();
    if (!removed) {
        reply.set_message("subscription not found for subscriber");
    }
    return reply;
}

PublishStats SubscriptionManager::publish(const rsp::proto::LogRecord& record,
                                          const DeliverCallback& deliver,
                                          const rsp::resource_manager::SchemaSnapshot* schemaSnapshot,
                                          const rsp::DateTime& now) {
    PublishStats stats;
    if (!record.has_payload() || record.payload().type_url().empty()) {
        return stats;
    }

    std::vector<SubscriptionSnapshot> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
            if (it->second.expiresAt <= now) {
                it = subscriptions_.erase(it);
                ++stats.expired_subscriptions;
                continue;
            }

            candidates.push_back(SubscriptionSnapshot{it->first,
                                                      it->second.subscriberNodeId,
                                                      it->second.payloadTypeUrl,
                                                      it->second.filter,
                                                      it->second.expiresAt});
            ++it;
        }
    }

    std::cerr << "[SM] publish: " << candidates.size() << " candidate(s), type="
              << record.payload().type_url() << "\n";

    if (schemaSnapshot == nullptr) {
        std::cerr << "[SM] publish: no schemaSnapshot, skipping delivery\n";
        return stats;
    }

    const auto* payloadDescriptor = schemaSnapshot->findMessageDescriptor(typeNameFromUrl(record.payload().type_url()));
    auto* payloadFactory = schemaSnapshot->messageFactory();
    if (payloadDescriptor == nullptr || payloadFactory == nullptr) {
        std::cerr << "[SM] publish: payloadDescriptor=" << (payloadDescriptor ? "ok" : "null")
                  << " payloadFactory=" << (payloadFactory ? "ok" : "null") << "\n";
        return stats;
    }

    const auto* payloadPrototype = payloadFactory->GetPrototype(payloadDescriptor);
    if (payloadPrototype == nullptr) {
        std::cerr << "[SM] publish: payloadPrototype null\n";
        return stats;
    }

    std::unique_ptr<google::protobuf::Message> unpackedPayload(payloadPrototype->New());
    if (unpackedPayload == nullptr || !unpackedPayload->ParseFromString(record.payload().value())) {
        std::cerr << "[SM] publish: payload parse failed\n";
        return stats;
    }

    std::set<rsp::NodeID> failedSubscribers;
    for (const auto& candidate : candidates) {
        if (failedSubscribers.count(candidate.subscriberNodeId) != 0) {
            continue;
        }

        if (candidate.payloadTypeUrl != record.payload().type_url()) {
            std::cerr << "[SM] publish: type_url mismatch: candidate=" << candidate.payloadTypeUrl
                      << " record=" << record.payload().type_url() << "\n";
            continue;
        }

        if (!evaluateFilterTree(candidate.filter, *unpackedPayload, schemaSnapshot)) {
            std::cerr << "[SM] publish: filter rejected candidate\n";
            continue;
        }

        ++stats.matched_subscriptions;

        rsp::proto::LogRecord deliverRecord = record;
        if (!deliverRecord.has_subscription_id()) {
            *deliverRecord.mutable_subscription_id() = guidToProto(candidate.id);
        }
        if (!deliverRecord.has_producer_node_id()) {
            *deliverRecord.mutable_producer_node_id() = nodeIdToProto(localNodeId_);
        }
        if (!deliverRecord.has_time_created()) {
            *deliverRecord.mutable_time_created() = dateTimeToProto(now);
        }

        rsp::proto::RSPMessage envelope;
        *envelope.mutable_source() = nodeIdToProto(localNodeId_);
        *envelope.mutable_destination() = nodeIdToProto(candidate.subscriberNodeId);
        *envelope.mutable_log_record() = deliverRecord;

        if (!deliver(envelope)) {
            std::cerr << "[SM] publish: deliver failed, removing subscriptions for node\n";
            failedSubscribers.insert(candidate.subscriberNodeId);
            stats.removed_subscriptions_on_failure += removeAllForNode(candidate.subscriberNodeId);
            continue;
        }

        std::cerr << "[SM] publish: delivered to " << candidate.subscriberNodeId.toString() << "\n";
        ++stats.delivered_messages;
    }

    return stats;
}

std::size_t SubscriptionManager::removeExpired(const rsp::DateTime& now) {
    std::size_t removed = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
        if (it->second.expiresAt <= now) {
            it = subscriptions_.erase(it);
            ++removed;
            continue;
        }
        ++it;
    }
    return removed;
}

std::size_t SubscriptionManager::removeAllForNode(const rsp::NodeID& subscriberNodeId) {
    std::size_t removed = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
        if (it->second.subscriberNodeId == subscriberNodeId) {
            it = subscriptions_.erase(it);
            ++removed;
            continue;
        }
        ++it;
    }
    return removed;
}

std::size_t SubscriptionManager::subscriptionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.size();
}

std::size_t SubscriptionManager::subscriptionCountForNode(const rsp::NodeID& subscriberNodeId) const {
    std::size_t count = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, subscription] : subscriptions_) {
        if (subscription.subscriberNodeId == subscriberNodeId) {
            ++count;
        }
    }
    return count;
}

bool SubscriptionManager::hasSubscription(const rsp::GUID& subscriptionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.find(subscriptionId) != subscriptions_.end();
}

}  // namespace rsp::logging