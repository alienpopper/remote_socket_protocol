#include "resource_manager/resource_manager.hpp"

#include <utility>

namespace rsp::resource_manager {

ResourceManager::ResourceManager() = default;

ResourceManager::ResourceManager(std::vector<std::unique_ptr<rsp::transport::Transport>> clientTransports)
    : clientTransports_(std::move(clientTransports)) {
}

int ResourceManager::run() const {
    return 0;
}

void ResourceManager::addClientTransport(std::unique_ptr<rsp::transport::Transport> transport) {
    clientTransports_.push_back(std::move(transport));
}

size_t ResourceManager::clientTransportCount() const {
    return clientTransports_.size();
}

}  // namespace rsp::resource_manager