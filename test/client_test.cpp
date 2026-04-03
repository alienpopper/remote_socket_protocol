#include "client/cpp/rsp_client.hpp"

#include "messages.pb.h"
#include "common/transport/transport_tcp.hpp"
#include "resource_manager/resource_manager.hpp"

#include "common/transport/transport.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
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

class MockConnection : public rsp::transport::Connection {
public:
    explicit MockConnection(int id) : id_(id) {
    }

    int send(const rsp::Buffer&) override {
        return 0;
    }

    int recv(rsp::Buffer&) override {
        return 0;
    }

    void close() override {
        closed_ = true;
    }

    int id() const {
        return id_;
    }

    bool isClosed() const {
        return closed_;
    }

private:
    int id_;
    bool closed_ = false;
};

class MockTransport : public rsp::transport::Transport {
public:
    rsp::transport::ConnectionHandle connect(const std::string& parameters) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lastParameters_ = parameters;
        ++connectCount_;
        activeConnection_ = std::make_shared<MockConnection>(connectCount_);
        return activeConnection_;
    }

    rsp::transport::ConnectionHandle reconnect() override {
        std::string parameters;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            parameters = lastParameters_;
        }

        if (parameters.empty()) {
            return nullptr;
        }

        return connect(parameters);
    }

    void stop() override {
        std::shared_ptr<rsp::transport::Connection> activeConnection;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeConnection = activeConnection_;
            activeConnection_.reset();
        }

        if (activeConnection != nullptr) {
            activeConnection->close();
        }
    }

    rsp::transport::ConnectionHandle connection() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return activeConnection_;
    }

    int connectCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connectCount_;
    }

    std::string lastParameters() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastParameters_;
    }

private:
    mutable std::mutex mutex_;
    int connectCount_ = 0;
    std::string lastParameters_;
    rsp::transport::ConnectionHandle activeConnection_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void setBytes(std::string* destination, const std::array<uint8_t, 16>& value) {
    destination->assign(reinterpret_cast<const char*>(value.data()), value.size());
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

rsp::proto::RSPMessage makeRouteUpdateMessage(const std::array<uint8_t, 16>& nodeIdBytes, uint32_t hopsAway) {
    rsp::proto::RSPMessage message;
    auto* entry = message.mutable_route()->add_entries();
    setBytes(entry->mutable_node_id()->mutable_value(), nodeIdBytes);
    entry->set_hops_away(hopsAway);
    return message;
}

std::string findListeningEndpoint(const std::shared_ptr<rsp::transport::TcpTransport>& serverTransport) {
    for (uint16_t port = 35000; port < 35100; ++port) {
        const std::string endpoint = std::string("127.0.0.1:") + std::to_string(port);
        if (serverTransport->listen(endpoint)) {
            return endpoint;
        }
    }

    throw std::runtime_error("failed to find an available TCP port for handshake test");
}

void testTcpAsciiHandshake() {
    auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair clientKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID clientNodeId = clientKeyPair.nodeID();

    std::promise<rsp::transport::ConnectionHandle> handshakePromise;
    std::future<rsp::transport::ConnectionHandle> handshakeFuture = handshakePromise.get_future();
    resourceManager.setNewConnectionCallback([&handshakePromise](const rsp::transport::ConnectionHandle& connection) {
        try {
            handshakePromise.set_value(connection);
        } catch (...) {
            handshakePromise.set_exception(std::current_exception());
        }
    });

    const std::string endpoint = findListeningEndpoint(serverTransport);

    rsp::client::RSPClient::Ptr client = rsp::client::RSPClient::create(std::move(clientKeyPair));
    const rsp::client::RSPClient::TransportID transportId = client->createTcpTransport();
    const rsp::transport::ConnectionHandle connection = client->connect(transportId, endpoint);
    require(connection != nullptr, "client should complete the ASCII and identity handshakes over TCP");

    require(handshakeFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "server handshake should complete");
    const rsp::transport::ConnectionHandle serverConnection = handshakeFuture.get();
    require(serverConnection != nullptr, "server should accept the authenticated transport");
    require(resourceManager.activeConnectionCount() == 1,
            "resource manager should be notified when a listening transport accepts a connection");
    require(resourceManager.activeEncodingCount() == 1,
            "resource manager should create an encoding for the accepted connection");
    require(resourceManager.pendingMessageCount() == 0,
        "authentication messages should not be exposed through the resource manager queue");

    const auto clientPeerNodeId = connection->peerNodeID();
    require(clientPeerNodeId.has_value(), "client transport should learn the server node id during authentication");
    require(clientPeerNodeId.value() == resourceManager.nodeId(),
        "client transport should store the resource manager node id");

    const auto serverPeerNodeId = serverConnection->peerNodeID();
    require(serverPeerNodeId.has_value(), "server transport should learn the client node id during authentication");
    require(serverPeerNodeId.value() == clientNodeId,
        "server transport should store the client node id");

    const std::array<uint8_t, 16> clientRouteNode = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                         0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    const rsp::proto::RSPMessage clientMessage = makeRouteUpdateMessage(clientRouteNode, 2);
    require(client->send(transportId, clientMessage), "client should send framed protobuf messages after authentication");
    require(waitForCondition([&resourceManager]() { return resourceManager.pendingMessageCount() == 1; }),
        "resource manager should receive and queue a decoded protobuf message");

    rsp::proto::RSPMessage queuedAtServer;
    require(resourceManager.tryDequeueMessage(queuedAtServer), "resource manager should expose queued decoded messages");
    require(queuedAtServer.has_route(), "resource manager should decode the client route update");
    require(queuedAtServer.route().entries_size() == 1, "resource manager should preserve route entries");
    require(queuedAtServer.route().entries(0).node_id().value() == clientMessage.route().entries(0).node_id().value(),
        "resource manager should preserve the route payload across framing");

    const std::array<uint8_t, 16> serverRouteNode = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                         0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
    const rsp::proto::RSPMessage serverMessage = makeRouteUpdateMessage(serverRouteNode, 5);
    require(resourceManager.sendToConnection(0, serverMessage),
        "resource manager should send framed protobuf messages through its active encoding");
    require(waitForCondition([&client]() { return client->pendingMessageCount() == 1; }),
        "client should receive and queue a decoded protobuf message");

    rsp::proto::RSPMessage queuedAtClient;
    require(client->tryDequeueMessage(queuedAtClient), "client should expose queued decoded protobuf messages");
    require(queuedAtClient.has_route(), "client should decode the server route update");
    require(queuedAtClient.route().entries_size() == 1, "client should preserve route entries");
    require(queuedAtClient.route().entries(0).node_id().value() == serverMessage.route().entries(0).node_id().value(),
        "client should preserve the route payload across framing");

    client->removeTransport(transportId);
    serverTransport->stop();
}

}  // namespace

int main() {
    try {
        rsp::client::RSPClient::Ptr client = rsp::client::RSPClient::create();
        require(client != nullptr, "client should be reference counted");
        require(!client->hasTransports(), "client should start without transports");
        require(client->transportCount() == 0, "client should start with zero transports");

        rsp::client::RSPClient::Ptr secondReference = client;
        require(secondReference.use_count() >= 2, "client should support shared ownership");

        std::shared_ptr<MockTransport> mockTransport = std::make_shared<MockTransport>();
        const rsp::client::RSPClient::TransportID mockTransportId = client->addTransport(mockTransport);
        const rsp::client::RSPClient::TransportID tcpTransportId = client->createTcpTransport();

        require(client->hasTransports(), "client should create transports");
        require(client->transportCount() == 2, "client should track multiple transports");
        require(client->hasTransport(mockTransportId), "client should track the mock transport");
        require(client->hasTransport(tcpTransportId), "client should track the TCP transport");
        require(mockTransportId != tcpTransportId, "client should return unique GUIDs for different transports");
        require(client->transport(mockTransportId) == mockTransport, "client should expose the stored transport");
        require(client->transport(rsp::GUID()) == nullptr, "client should reject unknown transport ids");

        std::vector<rsp::client::RSPClient::TransportID> transportIds = client->transportIds();
        require(transportIds.size() == 2, "client should enumerate transport ids");

        rsp::transport::TransportHandle transport = client->transport(mockTransportId);
        require(transport != nullptr, "client should return the managed transport");

        rsp::transport::ConnectionHandle firstConnection = transport->connect("peer.example:1234");
        require(firstConnection != nullptr, "client should connect a managed transport");
        require(transport->connection() == firstConnection, "transport should store the active connection");
        require(mockTransport->connectCount() == 1, "transport should connect once");
        require(mockTransport->lastParameters() == "peer.example:1234", "transport should persist connection parameters");

        rsp::transport::ConnectionHandle secondConnection = transport->reconnect();
        require(secondConnection != nullptr, "client should reconnect using the stored parameters");
        require(secondConnection != firstConnection, "reconnect should replace the previous connection");
        require(transport->connection() == secondConnection, "transport should update the active connection after reconnect");
        require(mockTransport->connectCount() == 2, "transport should reconnect through the transport");

        transport->stop();
        require(transport->connection() == nullptr, "stop should clear the active connection");

        std::atomic<bool> concurrentFailure = false;
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([transport, &concurrentFailure]() {
                try {
                    for (int iteration = 0; iteration < 50; ++iteration) {
                        rsp::transport::ConnectionHandle connection = transport->reconnect();
                        if (connection == nullptr) {
                            concurrentFailure = true;
                            return;
                        }

                        (void)transport->connection();
                        transport->stop();
                    }
                } catch (...) {
                    concurrentFailure = true;
                }
            });
        }

        for (std::thread& worker : threads) {
            worker.join();
        }

        require(!concurrentFailure.load(), "client operations should remain safe under concurrent access");

        require(client->removeTransport(mockTransportId), "client should remove an existing transport");
        require(!client->hasTransport(mockTransportId), "removed transport should no longer be tracked");
        require(!client->removeTransport(mockTransportId), "removing the same transport twice should fail");
        require(client->transportCount() == 1, "removing a transport should shrink the managed set");

        testTcpAsciiHandshake();

        std::cout << "client_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "client_test failed: " << exception.what() << '\n';
        return 1;
    }
}