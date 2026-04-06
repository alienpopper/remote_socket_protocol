#define RSPCLIENT_STATIC

#include "client/cpp/rsp_client.hpp"
#include "resource_service/resource_service.hpp"

#include "common/transport/transport_tcp.hpp"
#include "resource_manager/resource_manager.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

std::string findListeningEndpoint(const std::shared_ptr<rsp::transport::TcpTransport>& serverTransport) {
    for (uint16_t port = 35100; port < 35200; ++port) {
        const std::string endpoint = std::string("127.0.0.1:") + std::to_string(port);
        if (serverTransport->listen(endpoint)) {
            return endpoint;
        }
    }

    throw std::runtime_error("failed to find an available TCP port for resource service test");
}

std::string findSocketServerEndpoint(const std::shared_ptr<rsp::transport::TcpTransport>& serverTransport) {
    for (uint16_t port = 35200; port < 35300; ++port) {
        const std::string endpoint = std::string("127.0.0.1:") + std::to_string(port);
        if (serverTransport->listen(endpoint)) {
            return endpoint;
        }
    }

    throw std::runtime_error("failed to find an available TCP port for socket server test");
}

class TestSocketServer {
public:
    TestSocketServer() : transport_(std::make_shared<rsp::transport::TcpTransport>()) {
        transport_->setNewConnectionCallback([this](const rsp::transport::ConnectionHandle& connection) {
            try {
                connectionPromise_.set_value(connection);
            } catch (...) {
                connectionPromise_.set_exception(std::current_exception());
            }
        });
        endpoint_ = findSocketServerEndpoint(transport_);
    }

    ~TestSocketServer() {
        transport_->stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::string endpoint() const {
        return endpoint_;
    }

    void start(const std::string& greeting, const std::string& expectedPayload, const std::string& response) {
        worker_ = std::thread([this, greeting, expectedPayload, response]() {
            auto future = connectionPromise_.get_future();
            require(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                    "socket server should accept a TCP connection");
            const auto connection = future.get();
            require(connection != nullptr, "socket server should receive a connection handle");
            require(connection->sendAll(reinterpret_cast<const uint8_t*>(greeting.data()), static_cast<uint32_t>(greeting.size())),
                    "socket server should send greeting bytes");

            std::vector<uint8_t> payload(expectedPayload.size());
            require(connection->readExact(payload.data(), static_cast<uint32_t>(payload.size())),
                    "socket server should read the client payload");
            require(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()) == expectedPayload,
                    "socket server should receive the expected client payload");

            require(connection->sendAll(reinterpret_cast<const uint8_t*>(response.data()), static_cast<uint32_t>(response.size())),
                    "socket server should send response bytes");
            connection->close();
        });
    }

private:
    std::shared_ptr<rsp::transport::TcpTransport> transport_;
    std::promise<rsp::transport::ConnectionHandle> connectionPromise_;
    std::thread worker_;
    std::string endpoint_;
};

void testResourceServiceConnectsToResourceManager() {
    auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

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

    auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
    const auto connectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

    require(resourceService->hasConnections(), "resource service should track created connections");
    require(resourceService->hasConnection(connectionId), "resource service should expose the new connection id");
    require(resourceService->connectionCount() == 1, "resource service should report one live connection");
    require(resourceService->connectionIds().size() == 1,
            "resource service should enumerate its single live connection");

    require(handshakeFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "resource manager handshake pipeline should complete");
    const rsp::encoding::EncodingHandle serverEncoding = handshakeFuture.get();
    require(serverEncoding != nullptr, "resource manager should activate an encoding for the resource service");
    require(resourceManager.activeEncodingCount() == 1,
            "resource manager should expose one authenticated encoding");

    const auto resourceServicePeerNodeId = resourceService->peerNodeID(connectionId);
    require(resourceServicePeerNodeId.has_value(),
            "resource service should learn the resource manager node id during authentication");
    require(resourceServicePeerNodeId.value() == resourceManager.nodeId(),
            "resource service should store the resource manager node id");

    const auto serverPeerNodeId = serverEncoding->peerNodeID();
    require(serverPeerNodeId.has_value(),
            "resource manager encoding should learn the resource service node id during authentication");
    require(serverPeerNodeId.value() == resourceServiceNodeId,
            "resource manager encoding should store the resource service node id");

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 1; }),
            "resource manager should keep the resource service connection active");
    require(resourceManager.pendingMessageCount() == 0,
            "resource manager should not expose authentication traffic through its incoming message queue");

    require(resourceService->removeConnection(connectionId), "resource service should remove an existing connection");
    require(!resourceService->hasConnections(), "resource service should have no remaining connections");

    serverTransport->stop();
}

void testClientExchangesTcpDataThroughResourceService() {
    auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

    const std::string endpoint = findListeningEndpoint(serverTransport);
    const std::string transportSpec = std::string("tcp:") + endpoint;

    auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
    auto client = rsp::client::RSPClient::create();

    const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

    require(resourceService->hasConnection(resourceServiceConnectionId),
            "resource service should stay connected to the resource manager");
    require(client->hasConnection(clientConnectionId),
            "client should stay connected to the resource manager");
    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both the client and resource service");

    require(client->ping(resourceServiceNodeId),
            "client should ping the resource service through the resource manager");

    TestSocketServer socketServer;
    const std::string greeting = "server-greeting";
    const std::string clientPayload = "client-payload";
    const std::string serverResponse = "server-response";
    socketServer.start(greeting, clientPayload, serverResponse);

    const auto socketId = client->connectTCP(resourceServiceNodeId, socketServer.endpoint());
    require(socketId.has_value(), "client should receive a socket id from the resource service");

    const auto receivedGreeting = client->socketRecv(*socketId, static_cast<uint32_t>(greeting.size()));
    require(receivedGreeting.has_value(), "client should receive greeting bytes from the remote TCP server");
    require(*receivedGreeting == greeting, "client should receive the expected greeting bytes");

    require(client->socketSend(*socketId, clientPayload),
            "client should send payload bytes to the remote TCP server through the resource service");

    const auto receivedResponse = client->socketRecv(*socketId, static_cast<uint32_t>(serverResponse.size()));
    require(receivedResponse.has_value(), "client should receive response bytes from the remote TCP server");
    require(*receivedResponse == serverResponse, "client should receive the expected response bytes");

    require(client->socketClose(*socketId), "client should close the remote TCP socket through the resource service");

    require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its resource manager connection");
    require(client->removeConnection(clientConnectionId),
            "client should remove its resource manager connection");

    serverTransport->stop();
}

}  // namespace

int main() {
    try {
        testResourceServiceConnectsToResourceManager();
        testClientExchangesTcpDataThroughResourceService();
        std::cout << "resource service test passed" << std::endl;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "resource service test failed: " << exception.what() << std::endl;
        return 1;
    }
}