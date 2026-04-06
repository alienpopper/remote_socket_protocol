#include "resource_service/resource_service.hpp"

#include <utility>

namespace rsp::resource_service {

ResourceService::Ptr ResourceService::create() {
    return Ptr(new ResourceService(KeyPair::generateP256()));
}

ResourceService::Ptr ResourceService::create(KeyPair keyPair) {
    return Ptr(new ResourceService(std::move(keyPair)));
}

ResourceService::ResourceService(KeyPair keyPair)
    : rsp::client::full::RSPClient(std::move(keyPair)) {
}

}  // namespace rsp::resource_service