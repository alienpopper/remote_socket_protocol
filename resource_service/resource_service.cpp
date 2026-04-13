#include "resource_service/resource_service.hpp"

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

}  // namespace rsp::resource_service
