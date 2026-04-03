#pragma once

#include "common/ascii_handshake.hpp"
#include "common/node.hpp"
#include "common/transport/transport.hpp"

#include <memory>
#include <vector>

namespace rsp::resource_manager {

class ResourceManager : public rsp::RSPNode {
public:
    ResourceManager();
    explicit ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports);

    int run() const override;
    void addClientTransport(const rsp::transport::ListeningTransportHandle& transport);
    size_t clientTransportCount() const;
    bool performAsciiHandshake(const rsp::transport::ConnectionHandle& connection) const;

private:
    std::vector<rsp::transport::ListeningTransportHandle> clientTransports_;
};

}  // namespace rsp::resource_manager