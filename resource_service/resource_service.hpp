#pragma once

#include "client/cpp_full/rsp_client.hpp"

#include <memory>

namespace rsp::resource_service {

class ResourceService : public rsp::client::full::RSPClient {
public:
    using Ptr = std::shared_ptr<ResourceService>;
    using ConstPtr = std::shared_ptr<const ResourceService>;

    static Ptr create();
    static Ptr create(KeyPair keyPair);

    ~ResourceService() override = default;

    ResourceService(const ResourceService&) = delete;
    ResourceService& operator=(const ResourceService&) = delete;
    ResourceService(ResourceService&&) = delete;
    ResourceService& operator=(ResourceService&&) = delete;

private:
    explicit ResourceService(KeyPair keyPair);
};

}  // namespace rsp::resource_service