#include "client/cpp/rsp_client.hpp"

#include "common/transport/transport_tcp.hpp"
#include "resource_manager/resource_manager.hpp"

#include "common/transport/transport.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

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
    rsp::resource_manager::ResourceManager resourceManager({serverTransport});

    std::promise<bool> handshakePromise;
    std::future<bool> handshakeFuture = handshakePromise.get_future();
    resourceManager.setNewConnectionCallback([&handshakePromise](const rsp::transport::ConnectionHandle& connection) {
        try {
            const bool succeeded = (connection != nullptr);
            connection->close();
            handshakePromise.set_value(succeeded);
        } catch (...) {
            handshakePromise.set_exception(std::current_exception());
        }
    });

    const std::string endpoint = findListeningEndpoint(serverTransport);

    rsp::client::RSPClient::Ptr client = rsp::client::RSPClient::create();
    const rsp::client::RSPClient::TransportID transportId = client->createTcpTransport();
    const rsp::transport::ConnectionHandle connection = client->connect(transportId, endpoint);
    require(connection != nullptr, "client should complete the ASCII handshake over TCP");

    require(handshakeFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "server handshake should complete");
    require(handshakeFuture.get(), "server should accept the protobuf handshake");
        require(resourceManager.activeConnectionCount() == 1,
            "resource manager should be notified when a listening transport accepts a connection");

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