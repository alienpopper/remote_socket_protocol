#include "resource_manager/resource_manager.hpp"

#include "common/transport/transport_tcp.hpp"

#include <memory>
#include <vector>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::vector<std::unique_ptr<rsp::transport::Transport>> clientTransports;
    clientTransports.push_back(std::make_unique<rsp::transport::TcpTransport>());

    rsp::resource_manager::ResourceManager resourceManager(std::move(clientTransports));
    return resourceManager.run();
}