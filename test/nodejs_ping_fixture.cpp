#define RSPCLIENT_STATIC

#include "client/cpp_full/rsp_client.hpp"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/transport/transport_tcp.hpp"
#include "endorsement_service/endorsement_service.hpp"
#include "resource_manager/resource_manager.hpp"
#include "resource_service/bsd_sockets/resource_service_bsd_sockets.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> gStopRequested{false};

class TestResourceManager : public rsp::resource_manager::ResourceManager {
public:
    using rsp::resource_manager::ResourceManager::ResourceManager;

    rsp::NodeID nodeId() const {
        return keyPair().nodeID();
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool waitForCondition(const std::function<bool()>& condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return condition();
}

void signalHandler(int) {
    gStopRequested.store(true);
}

}  // namespace

int main(int argc, char* argv[]) {
    // Optional first argument: the IP address to advertise in the readiness JSON
    // for remote clients.  Defaults to 127.0.0.1 (loopback only).
    const std::string advertisedIp = (argc >= 2) ? std::string(argv[1]) : "127.0.0.1";

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    try {
        std::cerr << "[fixture] startup" << std::endl;
        auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
        require(serverTransport->listen("0.0.0.0:0"), "failed to start the RM TCP listener");
        const uint16_t port = serverTransport->listenedPort();
        std::cerr << "[fixture] RM listening on tcp:0.0.0.0:" << port << std::endl;

        TestResourceManager resourceManager({serverTransport});
        std::cerr << "[fixture] RM created" << std::endl;

        rsp::KeyPair endorsementServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID endorsementServiceNodeId = endorsementServiceKeyPair.nodeID();
        auto endorsementService = rsp::endorsement_service::EndorsementService::create(std::move(endorsementServiceKeyPair));

        rsp::KeyPair clientServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID clientServiceNodeId = clientServiceKeyPair.nodeID();
        auto clientService = rsp::client::full::RSPClient::create(std::move(clientServiceKeyPair));

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();
        auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));

        const std::string internalSpec = std::string("tcp:127.0.0.1:") + std::to_string(port);
        const std::string advertisedSpec = std::string("tcp:") + advertisedIp + ":" + std::to_string(port);

        const auto endorsementConnectionId =
            endorsementService->connectToResourceManager(internalSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        std::cerr << "[fixture] ES connected" << std::endl;
        const auto clientConnectionId =
            clientService->connectToResourceManager(internalSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        std::cerr << "[fixture] C++ client connected" << std::endl;
        const auto resourceServiceConnectionId =
            resourceService->connectToResourceManager(internalSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        std::cerr << "[fixture] RS connected" << std::endl;

        require(endorsementService->hasConnection(endorsementConnectionId),
            "endorsement service should keep its RM connection");
        require(clientService->hasConnection(clientConnectionId),
            "client service should keep its RM connection");
        require(resourceService->hasConnection(resourceServiceConnectionId),
            "resource service should keep its RM connection");
        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 3; },
                    std::chrono::seconds(5)),
            "RM should authenticate endorsement service, client service, and resource service");
        std::cerr << "[fixture] all 3 encodings authenticated" << std::endl;

        std::cout << "{"
                  << "\"transport_spec\":\"" << advertisedSpec << "\","
                  << "\"resource_manager_node_id\":\"" << resourceManager.nodeId().toString() << "\","
                  << "\"endorsement_service_node_id\":\"" << endorsementServiceNodeId.toString() << "\","
                  << "\"resource_service_node_id\":\"" << resourceServiceNodeId.toString() << "\","
                  << "\"client_service_node_id\":\"" << clientServiceNodeId.toString() << "\""
                  << "}" << std::endl;
        std::cerr << "[fixture] readiness JSON emitted" << std::endl;

        auto lastHeartbeat = std::chrono::steady_clock::now();

        while (!gStopRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto now = std::chrono::steady_clock::now();
            if (now - lastHeartbeat >= std::chrono::seconds(1)) {
                lastHeartbeat = now;
                std::cerr << "[fixture] heartbeat active_encodings=" << resourceManager.activeEncodingCount() << std::endl;
            }
        }

        std::cerr << "[fixture] shutdown requested" << std::endl;
        clientService->stop();
        endorsementService->stop();
        resourceService->stop();
        serverTransport->stop();
        std::cerr << "[fixture] stopped cleanly" << std::endl;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "nodejs_ping_fixture failed: " << exception.what() << std::endl;
        return 1;
    }
}