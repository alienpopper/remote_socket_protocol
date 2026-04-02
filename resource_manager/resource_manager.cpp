#include "resource_manager/resource_manager.hpp"

#include <utility>

namespace rsp::resource_manager {

ResourceManager::ResourceManager() = default;

ResourceManager::ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports)
    : clientTransports_(std::move(clientTransports)) {
}

int ResourceManager::run() const {
    return 0;
}

void ResourceManager::addClientTransport(const rsp::transport::ListeningTransportHandle& transport) {
    clientTransports_.push_back(transport);
}

size_t ResourceManager::clientTransportCount() const {
    return clientTransports_.size();
}

}  // namespace rsp::resource_manager