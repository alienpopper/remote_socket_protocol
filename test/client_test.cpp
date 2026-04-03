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

rsp::proto::RSPMessage makeChallengeRequestMessage(const std::array<uint8_t, 16>& sourceBytes,
                                                   const std::array<uint8_t, 16>& nonceBytes) {
    rsp::proto::RSPMessage message;
    setBytes(message.mutable_source()->mutable_value(), sourceBytes);
    setBytes(message.mutable_challenge_request()->mutable_nonce()->mutable_value(), nonceBytes);
    return message;
}

rsp::proto::RSPMessage makeChallengeReplyMessage(const std::array<uint8_t, 16>& sourceBytes,
                                                 const std::array<uint8_t, 16>& nonceBytes) {
    rsp::proto::RSPMessage message;
    setBytes(message.mutable_source()->mutable_value(), sourceBytes);
    setBytes(message.mutable_challenge_reply()->mutable_nonce()->mutable_value(), nonceBytes);
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
    rsp::resource_manager::ResourceManager resourceManager({serverTransport});

    std::promise<bool> handshakePromise;
    std::future<bool> handshakeFuture = handshakePromise.get_future();
    resourceManager.setNewConnectionCallback([&handshakePromise](const rsp::transport::ConnectionHandle& connection) {
        try {
            const bool succeeded = (connection != nullptr);
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
        require(resourceManager.activeEncodingCount() == 1,
            "resource manager should create an encoding for the accepted connection");

        const std::array<uint8_t, 16> clientSource = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                              0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
        const std::array<uint8_t, 16> clientNonce = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                             0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
        const rsp::proto::RSPMessage clientMessage = makeChallengeRequestMessage(clientSource, clientNonce);
        require(client->send(transportId, clientMessage), "client should send framed protobuf messages after the handshake");
        require(waitForCondition([&resourceManager]() { return resourceManager.pendingMessageCount() == 1; }),
            "resource manager should receive and queue a decoded protobuf message");

        rsp::proto::RSPMessage queuedAtServer;
        require(resourceManager.tryDequeueMessage(queuedAtServer), "resource manager should expose queued decoded messages");
        require(queuedAtServer.has_challenge_request(), "resource manager should decode the client challenge request");
        require(queuedAtServer.challenge_request().nonce().value() == clientMessage.challenge_request().nonce().value(),
            "resource manager should preserve the protobuf payload across framing");

        const std::array<uint8_t, 16> serverSource = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                              0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
        const std::array<uint8_t, 16> serverNonce = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                             0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};
        const rsp::proto::RSPMessage serverMessage = makeChallengeReplyMessage(serverSource, serverNonce);
        require(resourceManager.sendToConnection(0, serverMessage),
            "resource manager should send framed protobuf messages through its active encoding");
        require(waitForCondition([&client]() { return client->pendingMessageCount() == 1; }),
            "client should receive and queue a decoded protobuf reply");

        rsp::proto::RSPMessage queuedAtClient;
        require(client->tryDequeueMessage(queuedAtClient), "client should expose queued decoded protobuf messages");
        require(queuedAtClient.has_challenge_reply(), "client should decode the server challenge reply");
        require(queuedAtClient.challenge_reply().nonce().value() == serverMessage.challenge_reply().nonce().value(),
            "client should preserve the reply payload across framing");

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