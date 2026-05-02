#include "resource_service/resource_service.hpp"

#include "logging/logging.pb.h"
#include "logging/logging_desc.hpp"
#include "resource_manager/schema_registry.hpp"
#include "common/service_message.hpp"

#include <cstring>
#include <stdexcept>

namespace rsp::resource_service {

ResourceService::ResourceService(KeyPair keyPair)
    : rsp::client::full::RSPClient(std::move(keyPair)) {}

ResourceService::~ResourceService() = default;

ResourceService::ClientConnectionID ResourceService::connectToResourceManager(const std::string& transportSpec,
                                                                              const std::string& encoding) {
    const auto connectionId = rsp::client::full::RSPClient::connectToResourceManager(transportSpec, encoding);
    if (!sendResourceAdvertisement(connectionId)) {
        removeConnection(connectionId);
        throw std::runtime_error("failed to send resource advertisement");
    }

    enableReconnect(connectionId, [this](ClientConnectionID id) {
        sendResourceAdvertisement(id);
    });

    return connectionId;
}

rsp::proto::NodeId ResourceService::toProtoNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoNodeId.set_value(value);
    return protoNodeId;
}

void ResourceService::fillProtoAddress(const rsp::os::IPAddress& address, rsp::proto::Address* protoAddress) {
    if (protoAddress == nullptr) {
        return;
    }

    if (address.family == rsp::os::IPAddressFamily::IPv4) {
        protoAddress->set_ipv4(address.ipv4);
        protoAddress->clear_ipv6();
        return;
    }

    protoAddress->clear_ipv4();
    protoAddress->set_ipv6(address.ipv6.data(), address.ipv6.size());
}

bool ResourceService::sendResourceAdvertisement(ClientConnectionID connectionId) const {
    const auto destinationNodeId = peerNodeID(connectionId);
    if (!destinationNodeId.has_value()) {
        return false;
    }

    rsp::proto::RSPMessage message;
    *message.mutable_destination() = toProtoNodeId(*destinationNodeId);
    *message.mutable_resource_advertisement() = buildResourceAdvertisement();
    return sendOnConnection(connectionId, message);
}

namespace {

const rsp::resource_manager::SchemaSnapshot& generalLoggingSchema() {
    static const rsp::resource_manager::SchemaSnapshot snapshot = []() {
        rsp::proto::ServiceSchema schema;
        schema.set_proto_file_name("logging/logging.proto");
        schema.set_proto_file_descriptor_set(
            std::string(reinterpret_cast<const char*>(rsp::schema::kLoggingDescriptor),
                        rsp::schema::kLoggingDescriptorSize));
        return rsp::resource_manager::SchemaSnapshot({schema});
    }();
    return snapshot;
}

}  // namespace

void ResourceService::publishNodeStarted(const std::string& serviceType) {
    rsp::proto::NodeStartedEvent event;
    *event.mutable_node_id() = toProtoNodeId(keyPair().nodeID());
    event.set_service_type(serviceType);
    rsp::proto::LogRecord record;
    record.mutable_payload()->PackFrom(event, rsp::kTypeUrlPrefix);
    const auto& snap = generalLoggingSchema();
    publishLogRecord(record, &snap);
}

void ResourceService::publishNodeStopping(const std::string& serviceType) {
    rsp::proto::NodeStoppingEvent event;
    *event.mutable_node_id() = toProtoNodeId(keyPair().nodeID());
    event.set_service_type(serviceType);
    rsp::proto::LogRecord record;
    record.mutable_payload()->PackFrom(event, rsp::kTypeUrlPrefix);
    const auto& snap = generalLoggingSchema();
    publishLogRecord(record, &snap);
}

}  // namespace rsp::resource_service
