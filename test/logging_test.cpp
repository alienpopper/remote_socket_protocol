#include "common/logging/logging.hpp"
#include "common/service_message.hpp"
#include "logging/logging.pb.h"
#include "logging/logging_desc.hpp"

#include <google/protobuf/any.pb.h>

#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            throw std::runtime_error("CHECK failed: " #expr); \
        } \
    } while (0)

namespace {

int g_passed = 0;
int g_failed = 0;

void run(const char* name, void (*fn)()) {
    try {
        fn();
        std::cout << "PASS: " << name << "\n";
        ++g_passed;
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << name << ": " << e.what() << "\n";
        ++g_failed;
    }
}

rsp::resource_manager::SchemaSnapshot makeLoggingSchemaSnapshot() {
    rsp::proto::ServiceSchema schema;
    schema.set_proto_file_name("logging/logging.proto");
    schema.set_proto_file_descriptor_set(
        std::string(reinterpret_cast<const char*>(rsp::schema::kLoggingDescriptor),
                    sizeof(rsp::schema::kLoggingDescriptor)));
    return rsp::resource_manager::SchemaSnapshot({schema});
}

rsp::proto::LogASTFieldPath makePath(std::initializer_list<std::string> segments) {
    rsp::proto::LogASTFieldPath path;
    for (const auto& segment : segments) {
        path.add_segments(segment);
    }
    return path;
}

rsp::proto::LogASTMessageTree makeFieldEqualsString(std::initializer_list<std::string> segments,
                                                    const std::string& value) {
    rsp::proto::LogASTMessageTree tree;
    *tree.mutable_field_equals()->mutable_path() = makePath(segments);
    tree.mutable_field_equals()->mutable_value()->set_string_value(value);
    return tree;
}

rsp::proto::LogASTMessageTree makeFieldEqualsEnum(std::initializer_list<std::string> segments, int32_t value) {
    rsp::proto::LogASTMessageTree tree;
    *tree.mutable_field_equals()->mutable_path() = makePath(segments);
    tree.mutable_field_equals()->mutable_value()->set_enum_value(value);
    return tree;
}

rsp::proto::LogRecord makeLogRecord(const std::string& message) {
    rsp::proto::LogRecord record;

    rsp::proto::LogText text;
    text.set_message(message);
    text.add_tags("test");
    record.mutable_payload()->PackFrom(text, rsp::kTypeUrlPrefix);
    return record;
}

void testSubscribeRejectsZeroDuration() {
    rsp::logging::SubscriptionManager manager(rsp::NodeID(1, 2));
    rsp::proto::LogSubscribeRequest request;
    request.set_payload_type_url("type.rsp/rsp.proto.LogText");
    request.mutable_filter()->mutable_true_value();

    const auto reply = manager.subscribe(rsp::NodeID(3, 4), request,
                                         rsp::DateTime::fromMillisecondsSinceEpoch(1000));
    CHECK(reply.status() == rsp::proto::LOG_SUBSCRIPTION_STATUS_INVALID);
    CHECK(manager.subscriptionCount() == 0);
}

void testPublishOnlyDeliversMatchingSubscriber() {
    rsp::logging::SubscriptionManager manager(rsp::NodeID(10, 11));

    rsp::proto::LogSubscribeRequest matchingRequest;
    matchingRequest.set_payload_type_url("type.rsp/rsp.proto.LogText");
    *matchingRequest.mutable_filter() = makeFieldEqualsString({"message"}, "connected");
    matchingRequest.set_duration_ms(5000);
    const auto matchingReply = manager.subscribe(rsp::NodeID(20, 21), matchingRequest,
                                                 rsp::DateTime::fromMillisecondsSinceEpoch(1000));
    CHECK(matchingReply.status() == rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED);

    rsp::proto::LogSubscribeRequest nonMatchingRequest;
    nonMatchingRequest.set_payload_type_url("type.rsp/rsp.proto.LogText");
    *nonMatchingRequest.mutable_filter() = makeFieldEqualsString({"message"}, "other");
    nonMatchingRequest.set_duration_ms(5000);
    const auto nonMatchingReply = manager.subscribe(rsp::NodeID(30, 31), nonMatchingRequest,
                                                    rsp::DateTime::fromMillisecondsSinceEpoch(1000));
    CHECK(nonMatchingReply.status() == rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED);

    std::vector<rsp::proto::RSPMessage> delivered;
    const auto schemaSnapshot = makeLoggingSchemaSnapshot();
    const auto stats = manager.publish(
        makeLogRecord("connected"),
        [&delivered](const rsp::proto::RSPMessage& message) {
            delivered.push_back(message);
            return true;
        },
        &schemaSnapshot,
        rsp::DateTime::fromMillisecondsSinceEpoch(1200));

    CHECK(stats.matched_subscriptions == 1);
    CHECK(stats.delivered_messages == 1);
    CHECK(delivered.size() == 1);
    CHECK(delivered.front().destination().value() == rsp::logging::nodeIdToProto(rsp::NodeID(20, 21)).value());

    CHECK(delivered.front().has_log_record());
    const auto& unpacked = delivered.front().log_record();
    CHECK(unpacked.subscription_id().value() == matchingReply.subscription_id().value());
    rsp::proto::LogText deliveredText;
    CHECK(unpacked.payload().UnpackTo(&deliveredText));
    CHECK(deliveredText.message() == "connected");
}

void testPayloadFilterUsesDescriptorSnapshot() {
    rsp::logging::SubscriptionManager manager(rsp::NodeID(50, 51));
    rsp::proto::LogSubscribeRequest request;
    request.set_payload_type_url("type.rsp/rsp.proto.LogText");
    *request.mutable_filter() = makeFieldEqualsString({"message"}, "socket closed");
    request.set_duration_ms(5000);
    const auto reply = manager.subscribe(rsp::NodeID(60, 61), request,
                                         rsp::DateTime::fromMillisecondsSinceEpoch(1000));
    CHECK(reply.status() == rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED);

    std::vector<rsp::proto::RSPMessage> delivered;
    const auto schemaSnapshot = makeLoggingSchemaSnapshot();
    const auto stats = manager.publish(
        makeLogRecord("socket closed"),
        [&delivered](const rsp::proto::RSPMessage& message) {
            delivered.push_back(message);
            return true;
        },
        &schemaSnapshot,
        rsp::DateTime::fromMillisecondsSinceEpoch(1100));

    CHECK(stats.matched_subscriptions == 1);
    CHECK(stats.delivered_messages == 1);
    CHECK(delivered.size() == 1);
}

void testExpiredSubscriptionIsRemovedBeforeDelivery() {
    rsp::logging::SubscriptionManager manager(rsp::NodeID(70, 71));
    rsp::proto::LogSubscribeRequest request;
    request.set_payload_type_url("type.rsp/rsp.proto.LogText");
    request.mutable_filter()->mutable_true_value();
    request.set_duration_ms(25);
    const auto reply = manager.subscribe(rsp::NodeID(80, 81), request,
                                         rsp::DateTime::fromMillisecondsSinceEpoch(1000));
    CHECK(reply.status() == rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED);

    bool delivered = false;
    const auto stats = manager.publish(
        makeLogRecord("too late"),
        [&delivered](const rsp::proto::RSPMessage&) {
            delivered = true;
            return true;
        },
        nullptr,
        rsp::DateTime::fromMillisecondsSinceEpoch(1100));

    CHECK(!delivered);
    CHECK(stats.expired_subscriptions == 1);
    CHECK(manager.subscriptionCount() == 0);
}

void testDeliveryFailureRemovesAllSubscriptionsForSubscriber() {
    rsp::logging::SubscriptionManager manager(rsp::NodeID(90, 91));
    const rsp::NodeID subscriber(100, 101);

    rsp::proto::LogSubscribeRequest requestA;
    requestA.set_payload_type_url("type.rsp/rsp.proto.LogText");
    *requestA.mutable_filter() = makeFieldEqualsString({"message"}, "send failed");
    requestA.set_duration_ms(5000);
    CHECK(manager.subscribe(subscriber, requestA, rsp::DateTime::fromMillisecondsSinceEpoch(1000)).status() ==
          rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED);

    rsp::proto::LogSubscribeRequest requestB;
    requestB.set_payload_type_url("type.rsp/rsp.proto.LogText");
    *requestB.mutable_filter() = makeFieldEqualsString({"message"}, "send failed");
    requestB.set_duration_ms(5000);
    CHECK(manager.subscribe(subscriber, requestB, rsp::DateTime::fromMillisecondsSinceEpoch(1000)).status() ==
          rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED);

    std::size_t attempted = 0;
    const auto schemaSnapshot = makeLoggingSchemaSnapshot();
    const auto stats = manager.publish(
        makeLogRecord("send failed"),
        [&attempted](const rsp::proto::RSPMessage&) {
            ++attempted;
            return false;
        },
        &schemaSnapshot,
        rsp::DateTime::fromMillisecondsSinceEpoch(1100));

    CHECK(attempted == 1);
    CHECK(stats.removed_subscriptions_on_failure == 2);
    CHECK(manager.subscriptionCountForNode(subscriber) == 0);
}

}  // namespace

int main() {
    run("subscribe_rejects_zero_duration", testSubscribeRejectsZeroDuration);
    run("publish_only_delivers_matching_subscriber", testPublishOnlyDeliversMatchingSubscriber);
    run("payload_filter_uses_descriptor_snapshot", testPayloadFilterUsesDescriptorSnapshot);
    run("expired_subscription_removed_before_delivery", testExpiredSubscriptionIsRemovedBeforeDelivery);
    run("delivery_failure_removes_all_subscriptions", testDeliveryFailureRemovesAllSubscriptionsForSubscriber);

    std::cout << "Passed: " << g_passed << " Failed: " << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}