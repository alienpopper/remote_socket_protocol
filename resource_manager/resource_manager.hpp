#pragma once

#include "common/node.hpp"
#include "common/transport/transport.hpp"

#include <memory>
#include <vector>

namespace rsp::resource_manager {

class ResourceManager : public rsp::RSPNode {
public:
    ResourceManager();
    explicit ResourceManager(std::vector<std::unique_ptr<rsp::transport::Transport>> clientTransports);

    int run() const override;
    void addClientTransport(std::unique_ptr<rsp::transport::Transport> transport);
    size_t clientTransportCount() const;

private:
    std::vector<std::unique_ptr<rsp::transport::Transport>> clientTransports_;
};

}  // namespace rsp::resource_manager