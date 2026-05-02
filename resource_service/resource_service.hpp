#pragma once

#include "client/cpp_full/rsp_client.hpp"
#include "os/os_socket.hpp"

#include <memory>

namespace rsp::resource_service {

class ResourceService : public rsp::client::full::RSPClient {
public:
    using Ptr = std::shared_ptr<ResourceService>;
    using ConstPtr = std::shared_ptr<const ResourceService>;

    ~ResourceService() override;

    ResourceService(const ResourceService&) = delete;
    ResourceService& operator=(const ResourceService&) = delete;
    ResourceService(ResourceService&&) = delete;
    ResourceService& operator=(ResourceService&&) = delete;

    ClientConnectionID connectToResourceManager(const std::string& transportSpec, const std::string& encoding);

    void publishNodeStarted(const std::string& serviceType);
    void publishNodeStopping(const std::string& serviceType);

protected:
    explicit ResourceService(KeyPair keyPair);

    static rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId);
    static void fillProtoAddress(const rsp::os::IPAddress& address, rsp::proto::Address* protoAddress);

    virtual rsp::proto::ResourceAdvertisement buildResourceAdvertisement() const = 0;

private:
    bool sendResourceAdvertisement(ClientConnectionID connectionId) const;
};

}  // namespace rsp::resource_service
