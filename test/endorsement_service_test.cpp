#define RSPCLIENT_STATIC

#include "client/cpp/rsp_client.hpp"
#include "endorsement_service/endorsement_service.hpp"

#include "common/ascii_handshake.hpp"
#include "common/transport/transport_tcp.hpp"
#include "os/os_socket.hpp"
#include "resource_manager/resource_manager.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

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

bool waitForCondition(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

std::string findAvailableEndpoint() {
    const rsp::os::SocketHandle probe = rsp::os::createTcpListener("127.0.0.1", 0, 1);
    if (!rsp::os::isValidSocket(probe)) {
        throw std::runtime_error("failed to find an available TCP port for endorsement service test");
    }

    const uint16_t port = rsp::os::getSocketPort(probe);
    rsp::os::closeSocket(probe);
    return std::string("127.0.0.1:") + std::to_string(port);
}

std::string findListeningEndpoint(const std::shared_ptr<rsp::transport::TcpTransport>& serverTransport) {
    if (!serverTransport->listen("127.0.0.1:0")) {
        throw std::runtime_error("failed to listen on a random port for endorsement service test");
    }

    return std::string("127.0.0.1:") + std::to_string(serverTransport->listenedPort());
}

void testEndorsementServiceConnectsToResourceManager() {
    auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair esKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID esNodeId = esKeyPair.nodeID();

    std::promise<rsp::encoding::EncodingHandle> handshakePromise;
    std::future<rsp::encoding::EncodingHandle> handshakeFuture = handshakePromise.get_future();
    resourceManager.setNewEncodingCallback([&handshakePromise](const rsp::encoding::EncodingHandle& encoding) {
        try {
            handshakePromise.set_value(encoding);
        } catch (...) {
            handshakePromise.set_exception(std::current_exception());
        }
    });

    const std::string endpoint = findListeningEndpoint(serverTransport);
    const std::string transportSpec = std::string("tcp:") + endpoint;

    auto es = rsp::endorsement_service::EndorsementService::create(std::move(esKeyPair));
    const auto connectionId = es->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

    require(es->hasConnections(), "endorsement service should track created connections");
    require(es->hasConnection(connectionId), "endorsement service should expose the new connection id");
    require(es->connectionCount() == 1, "endorsement service should report one live connection");
    require(es->connectionIds().size() == 1, "endorsement service should enumerate its single live connection");

    require(handshakeFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "resource manager handshake pipeline should complete for endorsement service");
    const rsp::encoding::EncodingHandle serverEncoding = handshakeFuture.get();
    require(serverEncoding != nullptr, "resource manager should activate an encoding for the endorsement service");
    require(resourceManager.activeEncodingCount() == 1,
            "resource manager should expose one authenticated encoding");

    const auto esPeerNodeId = es->peerNodeID(connectionId);
    require(esPeerNodeId.has_value(),
            "endorsement service should learn the resource manager node id during authentication");
    require(esPeerNodeId.value() == resourceManager.nodeId(),
            "endorsement service should store the resource manager node id");

    const auto serverPeerNodeId = serverEncoding->peerNodeID();
    require(serverPeerNodeId.has_value(),
            "resource manager encoding should learn the endorsement service node id during authentication");
    require(serverPeerNodeId.value() == esNodeId,
            "resource manager encoding should store the endorsement service node id");

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 1; }),
            "resource manager should keep the endorsement service connection active");
    require(resourceManager.pendingMessageCount() == 0,
            "resource manager should not expose authentication traffic through its incoming message queue");

    require(es->removeConnection(connectionId), "endorsement service should remove an existing connection");
    require(!es->hasConnections(), "endorsement service should have no remaining connections");

    serverTransport->stop();
}

void testClientPingsEndorsementService() {
    auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair esKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID esNodeId = esKeyPair.nodeID();

    const std::string endpoint = findListeningEndpoint(serverTransport);
    const std::string transportSpec = std::string("tcp:") + endpoint;

    auto es = rsp::endorsement_service::EndorsementService::create(std::move(esKeyPair));
    auto client = rsp::client::RSPClient::create();

    const auto esConnectionId = es->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

    require(es->hasConnection(esConnectionId),
            "endorsement service should stay connected to the resource manager");
    require(client->hasConnection(clientConnectionId),
            "client should stay connected to the resource manager");
    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both the client and endorsement service");

    require(client->ping(esNodeId),
            "client should ping the endorsement service through the resource manager");

    require(es->removeConnection(esConnectionId),
            "endorsement service should remove its resource manager connection");
    require(client->removeConnection(clientConnectionId),
            "client should remove its resource manager connection");

    serverTransport->stop();
}

}  // namespace

int main() {
    try {
        testEndorsementServiceConnectsToResourceManager();
        std::cout << "testEndorsementServiceConnectsToResourceManager: PASSED\n";

        testClientPingsEndorsementService();
        std::cout << "testClientPingsEndorsementService: PASSED\n";
    } catch (const std::exception& ex) {
        std::cerr << "FAILED: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
