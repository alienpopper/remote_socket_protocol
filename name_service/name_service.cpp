#include "name_service/name_service.hpp"

#include "common/message_queue/mq_signing.hpp"
#include "common/service_message.hpp"
#include "resource_service/schema_helpers.hpp"

#include "name_service/name_service.pb.h"
#include "name_service/name_service_desc.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <utility>

namespace rsp::name_service {

static constexpr double kRecordTtlSeconds = 300.0; // 5 minutes

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

using RecordKey = std::tuple<std::string, std::string, std::string>;

RecordKey makeKey(const std::string& name, const std::string& ownerBytes, const std::string& typeBytes) {
    return {name, ownerBytes, typeBytes};
}

RecordKey makeKey(const rsp::proto::NameRecord& record) {
    return makeKey(record.name(),
                   record.owner().value(),
                   record.type().value());
}

rsp::proto::NameRecord toProto(const NameService::StoredRecord& stored) {
    rsp::proto::NameRecord record;
    record.set_name(stored.name);
    record.mutable_owner()->set_value(stored.ownerBytes);
    record.mutable_type()->set_value(stored.typeBytes);
    record.mutable_value()->set_value(stored.valueBytes);
    record.mutable_expires_at()->set_milliseconds_since_epoch(stored.expiresAt.millisecondsSinceEpoch());
    return record;
}

void sortByExpiresAtDescending(rsp::proto::NameReadReply& reply) {
    auto* recs = reply.mutable_records();
    std::sort(recs->begin(), recs->end(), [](const rsp::proto::NameRecord& a, const rsp::proto::NameRecord& b) {
        return a.expires_at().milliseconds_since_epoch() > b.expires_at().milliseconds_since_epoch();
    });
}

void sortByExpiresAtDescending(rsp::proto::NameQueryReply& reply) {
    auto* recs = reply.mutable_records();
    std::sort(recs->begin(), recs->end(), [](const rsp::proto::NameRecord& a, const rsp::proto::NameRecord& b) {
        return a.expires_at().milliseconds_since_epoch() > b.expires_at().milliseconds_since_epoch();
    });
}

}  // namespace

NameService::Ptr NameService::create() {
    return Ptr(new NameService(KeyPair::generateP256()));
}

NameService::Ptr NameService::create(KeyPair keyPair) {
    return Ptr(new NameService(std::move(keyPair)));
}

NameService::NameService(KeyPair keyPair)
    : ResourceService(std::move(keyPair)) {
    sweepThread_ = std::thread([this]() { runSweepThread(); });
    pingCheckThread_ = std::thread([this]() { runPingCheckThread(); });
}

NameService::~NameService() {
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        sweepStopping_ = true;
    }
    sweepCv_.notify_all();
    if (sweepThread_.joinable()) {
        sweepThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(pingCheckMutex_);
        pingCheckStopping_ = true;
    }
    pingCheckCv_.notify_all();
    if (pingCheckThread_.joinable()) {
        pingCheckThread_.join();
    }
}

rsp::proto::ResourceAdvertisement NameService::buildResourceAdvertisement() const {
    rsp::proto::ResourceAdvertisement advertisement;

    *advertisement.add_schemas() = rsp::resource_service::buildServiceSchema(
        "name_service.proto",
        rsp::schema::kNameServiceDescriptor,
        rsp::schema::kNameServiceDescriptorSize,
        1,
        {
            "type.rsp/rsp.proto.NameCreateRequest",
            "type.rsp/rsp.proto.NameReadRequest",
            "type.rsp/rsp.proto.NameUpdateRequest",
            "type.rsp/rsp.proto.NameDeleteRequest",
            "type.rsp/rsp.proto.NameQueryRequest",
            "type.rsp/rsp.proto.NameRefreshRequest",
        });

    return advertisement;
}

bool NameService::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (rsp::hasServiceMessage<rsp::proto::NameCreateRequest>(message)) {
        return handleCreateRequest(message);
    }
    if (rsp::hasServiceMessage<rsp::proto::NameReadRequest>(message)) {
        return handleReadRequest(message);
    }
    if (rsp::hasServiceMessage<rsp::proto::NameUpdateRequest>(message)) {
        return handleUpdateRequest(message);
    }
    if (rsp::hasServiceMessage<rsp::proto::NameDeleteRequest>(message)) {
        return handleDeleteRequest(message);
    }
    if (rsp::hasServiceMessage<rsp::proto::NameQueryRequest>(message)) {
        return handleQueryRequest(message);
    }
    if (rsp::hasServiceMessage<rsp::proto::NameRefreshRequest>(message)) {
        return handleRefreshRequest(message);
    }
    return false;
}

bool NameService::handleCreateRequest(const rsp::proto::RSPMessage& message) {
    rsp::proto::NameCreateRequest request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    const auto& record = request.record();
    const auto key = makeKey(record);

    // Find any existing records for (name, type) under a different owner.
    // If exactly one exists, we'll ping-check it after inserting the new record.
    std::optional<PingCheckItem> pingCheck;

    rsp::proto::NameCreateReply reply;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        if (records_.count(key) > 0) {
            reply.set_status(rsp::proto::NAME_DUPLICATE);
            reply.set_message("record with (name, owner, type) already exists");
        } else {
            // Count existing records for the same (name, type) under different owners.
            std::vector<RecordKey> sameNameType;
            for (const auto& [k, stored] : records_) {
                if (stored.name == record.name() &&
                    stored.typeBytes == record.type().value() &&
                    stored.ownerBytes != record.owner().value()) {
                    sameNameType.push_back(k);
                }
            }
            if (sameNameType.size() == 1) {
                const auto& oldKey = sameNameType[0];
                pingCheck = PingCheckItem{oldKey, records_.at(oldKey).ownerBytes};
            }

            StoredRecord stored;
            stored.name = record.name();
            stored.ownerBytes = record.owner().value();
            stored.typeBytes = record.type().value();
            stored.valueBytes = record.value().value();
            stored.expiresAt = rsp::DateTime();
            stored.expiresAt += kRecordTtlSeconds;
            records_[key] = std::move(stored);
            reply.set_status(rsp::proto::NAME_SUCCESS);
        }
    }

    rsp::proto::RSPMessage response;
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
    if (!requesterNodeId.has_value()) {
        return false;
    }
    *response.mutable_destination() = toProtoNodeId(*requesterNodeId);
    rsp::packServiceMessage(response, reply);
    const bool sent = send(response);

    // Enqueue ping check after sending reply (cannot block the message worker thread).
    if (pingCheck.has_value()) {
        std::lock_guard<std::mutex> lock(pingCheckMutex_);
        pingCheckQueue_.push_back(std::move(*pingCheck));
        pingCheckCv_.notify_one();
    }

    return sent;
}

bool NameService::handleReadRequest(const rsp::proto::RSPMessage& message) {
    rsp::proto::NameReadRequest request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    rsp::proto::NameReadReply reply;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        bool found = false;
        const rsp::DateTime now;
        for (const auto& [key, stored] : records_) {
            if (stored.name != request.name()) {
                continue;
            }
            if (request.has_owner() && stored.ownerBytes != request.owner().value()) {
                continue;
            }
            if (request.has_type() && stored.typeBytes != request.type().value()) {
                continue;
            }
            if (stored.expiresAt <= now) {
                continue;
            }
            *reply.add_records() = toProto(stored);
            found = true;
        }
        reply.set_status(found ? rsp::proto::NAME_SUCCESS : rsp::proto::NAME_NOT_FOUND);
    }
    sortByExpiresAtDescending(reply);

    rsp::proto::RSPMessage response;
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
    if (!requesterNodeId.has_value()) {
        return false;
    }
    *response.mutable_destination() = toProtoNodeId(*requesterNodeId);
    rsp::packServiceMessage(response, reply);
    return send(response);
}

bool NameService::handleUpdateRequest(const rsp::proto::RSPMessage& message) {
    rsp::proto::NameUpdateRequest request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    const auto key = makeKey(request.name(),
                             request.owner().value(),
                             request.type().value());

    rsp::proto::NameUpdateReply reply;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        auto it = records_.find(key);
        if (it == records_.end()) {
            reply.set_status(rsp::proto::NAME_NOT_FOUND);
            reply.set_message("no matching record");
        } else {
            it->second.valueBytes = request.new_value().value();
            reply.set_status(rsp::proto::NAME_SUCCESS);
        }
    }

    rsp::proto::RSPMessage response;
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
    if (!requesterNodeId.has_value()) {
        return false;
    }
    *response.mutable_destination() = toProtoNodeId(*requesterNodeId);
    rsp::packServiceMessage(response, reply);
    return send(response);
}

bool NameService::handleDeleteRequest(const rsp::proto::RSPMessage& message) {
    rsp::proto::NameDeleteRequest request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    const auto key = makeKey(request.name(),
                             request.owner().value(),
                             request.type().value());

    rsp::proto::NameDeleteReply reply;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        auto it = records_.find(key);
        if (it == records_.end()) {
            reply.set_status(rsp::proto::NAME_NOT_FOUND);
            reply.set_message("no matching record");
        } else {
            records_.erase(it);
            reply.set_status(rsp::proto::NAME_SUCCESS);
        }
    }

    rsp::proto::RSPMessage response;
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
    if (!requesterNodeId.has_value()) {
        return false;
    }
    *response.mutable_destination() = toProtoNodeId(*requesterNodeId);
    rsp::packServiceMessage(response, reply);
    return send(response);
}

bool NameService::handleQueryRequest(const rsp::proto::RSPMessage& message) {
    rsp::proto::NameQueryRequest request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    rsp::proto::NameQueryReply reply;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        uint32_t count = 0;
        const rsp::DateTime now;
        for (const auto& [key, stored] : records_) {
            if (request.has_name_prefix() && !request.name_prefix().empty()) {
                if (stored.name.find(request.name_prefix()) != 0) {
                    continue;
                }
            }
            if (request.has_owner() && stored.ownerBytes != request.owner().value()) {
                continue;
            }
            if (request.has_type() && stored.typeBytes != request.type().value()) {
                continue;
            }
            if (stored.expiresAt <= now) {
                continue;
            }
            *reply.add_records() = toProto(stored);
            ++count;
            if (request.max_records() > 0 && count >= request.max_records()) {
                break;
            }
        }
    }
    sortByExpiresAtDescending(reply);

    rsp::proto::RSPMessage response;
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
    if (!requesterNodeId.has_value()) {
        return false;
    }
    *response.mutable_destination() = toProtoNodeId(*requesterNodeId);
    rsp::packServiceMessage(response, reply);
    return send(response);
}

bool NameService::handleRefreshRequest(const rsp::proto::RSPMessage& message) {
    rsp::proto::NameRefreshRequest request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    const auto key = makeKey(request.name(),
                             request.owner().value(),
                             request.type().value());

    rsp::proto::NameRefreshReply reply;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        auto it = records_.find(key);
        if (it == records_.end()) {
            reply.set_status(rsp::proto::NAME_NOT_FOUND);
            reply.set_message("no matching record");
        } else {
            it->second.expiresAt = rsp::DateTime();
            it->second.expiresAt += kRecordTtlSeconds;
            reply.set_status(rsp::proto::NAME_SUCCESS);
        }
    }

    rsp::proto::RSPMessage response;
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
    if (!requesterNodeId.has_value()) {
        return false;
    }
    *response.mutable_destination() = toProtoNodeId(*requesterNodeId);
    rsp::packServiceMessage(response, reply);
    return send(response);
}

void NameService::runSweepThread() {
    static constexpr auto kSweepInterval = std::chrono::seconds(60);
    std::unique_lock<std::mutex> lock(dbMutex_);
    while (true) {
        sweepCv_.wait_for(lock, kSweepInterval, [this] { return sweepStopping_; });
        if (sweepStopping_) break;
        const rsp::DateTime now;
        for (auto it = records_.begin(); it != records_.end(); ) {
            if (it->second.expiresAt < now) {
                it = records_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void NameService::runPingCheckThread() {
    while (true) {
        PingCheckItem item;
        {
            std::unique_lock<std::mutex> lock(pingCheckMutex_);
            pingCheckCv_.wait(lock, [this] { return pingCheckStopping_ || !pingCheckQueue_.empty(); });
            if (pingCheckStopping_) break;
            item = std::move(pingCheckQueue_.front());
            pingCheckQueue_.pop_front();
        }

        // Reconstruct the NodeID from the raw bytes stored in ownerBytes.
        rsp::NodeID oldOwner;
        if (item.oldOwnerBytes.size() == 16) {
            uint64_t high = 0, low = 0;
            std::memcpy(&high, item.oldOwnerBytes.data(), 8);
            std::memcpy(&low,  item.oldOwnerBytes.data() + 8, 8);
            oldOwner = rsp::NodeID(high, low);
        }

        // ping() blocks on pingCheckThread, not on the IncomingMessageQueue worker.
        const bool alive = (oldOwner != rsp::NodeID{}) && ping(oldOwner);
        if (!alive) {
            std::lock_guard<std::mutex> lock(dbMutex_);
            const auto it = records_.find(item.oldRecordKey);
            if (it != records_.end()) {
                it->second.expiresAt = rsp::DateTime(0.0); // immediately expired
            }
        }
    }
}

}  // namespace rsp::name_service
