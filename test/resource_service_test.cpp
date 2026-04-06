#define RSPCLIENT_STATIC

#include "client/cpp/rsp_client.hpp"
#include "resource_service/resource_service.hpp"

#include "common/transport/transport_tcp.hpp"
#include "resource_manager/resource_manager.hpp"
#include "os/os_socket.hpp"

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

bool sendAllSocket(const rsp::os::SocketHandle socketHandle, const std::string& data) {
    std::size_t bytesSent = 0;
    while (bytesSent < data.size()) {
        const int result = rsp::os::sendSocket(socketHandle,
                                               reinterpret_cast<const uint8_t*>(data.data()) + bytesSent,
                                               static_cast<uint32_t>(data.size() - bytesSent));
        if (result <= 0) {
            return false;
        }

        bytesSent += static_cast<std::size_t>(result);
    }

    return true;
}

std::optional<std::string> recvExactSocket(const rsp::os::SocketHandle socketHandle, std::size_t bytesToRead) {
    std::string data(bytesToRead, '\0');
    std::size_t totalRead = 0;
    while (totalRead < bytesToRead) {
        const int result = rsp::os::recvSocket(socketHandle,
                                               reinterpret_cast<uint8_t*>(data.data()) + totalRead,
                                               static_cast<uint32_t>(bytesToRead - totalRead));
        if (result <= 0) {
            return std::nullopt;
        }

        totalRead += static_cast<std::size_t>(result);
    }

    return data;
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

std::string findAvailableEndpoint(uint16_t firstPort, uint16_t lastPort) {
    for (uint16_t port = firstPort; port < lastPort; ++port) {
        auto probeTransport = std::make_shared<rsp::transport::TcpTransport>();
        const std::string endpoint = std::string("127.0.0.1:") + std::to_string(port);
        if (probeTransport->listen(endpoint)) {
            probeTransport->stop();
            return endpoint;
        }
    }

    throw std::runtime_error("failed to find an available TCP port");
}

std::optional<rsp::GUID> fromProtoSocketId(const rsp::proto::SocketID& socketId) {
    if (socketId.value().size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, socketId.value().data(), sizeof(high));
    std::memcpy(&low, socketId.value().data() + sizeof(high), sizeof(low));
    return rsp::GUID(high, low);
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
            try {
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
            } catch (...) {
            workerException_ = std::current_exception();
            }
        });
    }

        void wait() {
        if (worker_.joinable()) {
            worker_.join();
        }

        if (workerException_ != nullptr) {
            std::rethrow_exception(workerException_);
        }
        }

private:
    std::shared_ptr<rsp::transport::TcpTransport> transport_;
    std::promise<rsp::transport::ConnectionHandle> connectionPromise_;
    std::thread worker_;
    std::string endpoint_;
        std::exception_ptr workerException_;
};

class PeriodicTestSocketServer {
public:
    PeriodicTestSocketServer() : transport_(std::make_shared<rsp::transport::TcpTransport>()) {
        transport_->setNewConnectionCallback([this](const rsp::transport::ConnectionHandle& connection) {
            try {
                connectionPromise_.set_value(connection);
            } catch (...) {
                connectionPromise_.set_exception(std::current_exception());
            }
        });
        endpoint_ = findSocketServerEndpoint(transport_);
    }

    ~PeriodicTestSocketServer() {
        transport_->stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::string endpoint() const {
        return endpoint_;
    }

    void start(const std::vector<std::string>& messages, uint32_t intervalMilliseconds) {
        worker_ = std::thread([this, messages, intervalMilliseconds]() {
            try {
                auto future = connectionPromise_.get_future();
                require(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                        "periodic socket server should accept a TCP connection");
                const auto connection = future.get();
                require(connection != nullptr, "periodic socket server should receive a connection handle");
                for (const auto& message : messages) {
                    require(connection->sendAll(reinterpret_cast<const uint8_t*>(message.data()), static_cast<uint32_t>(message.size())),
                            "periodic socket server should send periodic bytes");
                    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMilliseconds));
                }
                connection->close();
            } catch (...) {
                workerException_ = std::current_exception();
            }
        });
    }

    void wait() {
        if (worker_.joinable()) {
            worker_.join();
        }

        if (workerException_ != nullptr) {
            std::rethrow_exception(workerException_);
        }
    }

private:
    std::shared_ptr<rsp::transport::TcpTransport> transport_;
    std::promise<rsp::transport::ConnectionHandle> connectionPromise_;
    std::thread worker_;
    std::string endpoint_;
    std::exception_ptr workerException_;
};

class TestSocketClientPeer {
public:
    ~TestSocketClientPeer() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void start(const std::string& endpoint,
               const std::string& greeting,
               const std::string& expectedPayload,
               const std::string& response) {
        worker_ = std::thread([this, endpoint, greeting, expectedPayload, response]() {
            try {
            auto transport = std::make_shared<rsp::transport::TcpTransport>();
            const auto connection = transport->connect(endpoint);
            require(connection != nullptr, "socket client peer should connect to the listener endpoint");
            require(connection->sendAll(reinterpret_cast<const uint8_t*>(greeting.data()), static_cast<uint32_t>(greeting.size())),
                "socket client peer should send greeting bytes");

            std::vector<uint8_t> payload(expectedPayload.size());
            require(connection->readExact(payload.data(), static_cast<uint32_t>(payload.size())),
                "socket client peer should read the accepted socket payload");
            require(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()) == expectedPayload,
                "socket client peer should receive the expected accepted socket payload");

            require(connection->sendAll(reinterpret_cast<const uint8_t*>(response.data()), static_cast<uint32_t>(response.size())),
                "socket client peer should send response bytes");
            connection->close();
            transport->stop();
            } catch (...) {
            workerException_ = std::current_exception();
            }
        });
    }

        void wait() {
        if (worker_.joinable()) {
            worker_.join();
        }

        if (workerException_ != nullptr) {
            std::rethrow_exception(workerException_);
        }
        }

private:
    std::thread worker_;
        std::exception_ptr workerException_;
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
    socketServer.wait();

    require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its resource manager connection");
    require(client->removeConnection(clientConnectionId),
            "client should remove its resource manager connection");

    serverTransport->stop();
}

void testClientReceivesAsyncSocketDataThroughResourceService() {
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

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for async socket test");
    require(client->ping(resourceServiceNodeId),
            "client should ping the resource service before async socket test");

    PeriodicTestSocketServer socketServer;
    const std::vector<std::string> periodicMessages = {"tick-1", "tick-2", "tick-3"};
    std::string expectedStream;
    for (const auto& message : periodicMessages) {
        expectedStream += message;
    }
    socketServer.start(periodicMessages, 100);

    const auto socketId = client->connectTCP(resourceServiceNodeId, socketServer.endpoint(), 0, 0, 0, true);
    require(socketId.has_value(), "client should receive a socket id for async socket connection");

    bool sawAsyncSocketReply = false;
    std::string receivedStream;
    const auto recvReply = client->socketRecvEx(*socketId, 32);
    require(recvReply.has_value(), "client should receive a socket reply when socket_recv is used on an async socket");
    if (recvReply->error() == rsp::proto::ASYNC_SOCKET) {
        sawAsyncSocketReply = true;
    } else if (recvReply->error() == rsp::proto::SOCKET_DATA && recvReply->has_data()) {
        receivedStream += recvReply->data();
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!sawAsyncSocketReply || receivedStream.size() < expectedStream.size()) &&
           std::chrono::steady_clock::now() < deadline) {
        rsp::proto::SocketReply reply;
        if (client->tryDequeueSocketReply(reply)) {
            if (reply.error() == rsp::proto::ASYNC_SOCKET) {
                sawAsyncSocketReply = true;
            } else if (reply.error() == rsp::proto::SOCKET_DATA && reply.has_data()) {
                receivedStream += reply.data();
            }
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    require(sawAsyncSocketReply,
            "socket_recv on an async socket should eventually reply with ASYNC_SOCKET");
    require(receivedStream == expectedStream,
        "client should receive the expected periodic async socket payload stream in order");

    require(client->socketClose(*socketId), "client should close the async socket through the resource service");
    socketServer.wait();
    require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its async test connection");
    require(client->removeConnection(clientConnectionId),
            "client should remove its async test connection");

    serverTransport->stop();
}

void testClientExchangesTcpDataThroughNativeSocketBridge() {
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

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for native socket bridge test");
    require(client->ping(resourceServiceNodeId),
            "client should ping the resource service before the native socket bridge test");

    TestSocketServer socketServer;
    const std::string greeting = "bridge-greeting";
    const std::string clientPayload = "bridge-payload";
    const std::string serverResponse = "bridge-response";
    socketServer.start(greeting, clientPayload, serverResponse);

    const auto localSocket = client->connectTCPSocket(resourceServiceNodeId, socketServer.endpoint());
    require(localSocket.has_value(), "client should receive a local native socket for the TCP bridge");

    const auto receivedGreeting = recvExactSocket(*localSocket, greeting.size());
    require(receivedGreeting.has_value(), "client should receive bridge greeting bytes via the native socket");
    require(*receivedGreeting == greeting, "client should receive the expected bridge greeting bytes");

    require(sendAllSocket(*localSocket, clientPayload),
            "client should send payload bytes via the native socket bridge");

    const auto receivedResponse = recvExactSocket(*localSocket, serverResponse.size());
    require(receivedResponse.has_value(), "client should receive bridge response bytes via the native socket");
    require(*receivedResponse == serverResponse, "client should receive the expected bridge response bytes");

    rsp::os::closeSocket(*localSocket);
    socketServer.wait();
    require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its native socket bridge test connection");
    require(client->removeConnection(clientConnectionId),
            "client should remove its native socket bridge test connection");

    serverTransport->stop();
}

    void testClientAcceptsTcpConnectionThroughResourceService() {
        auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

        const std::string endpoint = findListeningEndpoint(serverTransport);
        const std::string transportSpec = std::string("tcp:") + endpoint;
        const std::string listenerEndpoint = findAvailableEndpoint(35300, 35400);

        auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for listen/accept test");
        require(client->ping(resourceServiceNodeId),
            "client should ping the resource service before listen/accept test");

        const auto listenSocketId = client->listenTCP(resourceServiceNodeId, listenerEndpoint);
        require(listenSocketId.has_value(), "client should receive a listening socket id from the resource service");

        TestSocketClientPeer peer;
        const std::string greeting = "accept-greeting";
        const std::string clientPayload = "accept-payload";
        const std::string response = "accept-response";
        peer.start(listenerEndpoint, greeting, clientPayload, response);

        const auto childSocketId = client->acceptTCP(*listenSocketId, std::nullopt, 5000);
        require(childSocketId.has_value(), "client should accept an inbound TCP connection through the resource service");

        const auto receivedGreeting = client->socketRecv(*childSocketId, static_cast<uint32_t>(greeting.size()));
        require(receivedGreeting.has_value(), "accepted socket should receive greeting bytes from the peer");
        require(*receivedGreeting == greeting, "accepted socket should receive the expected greeting bytes");

        require(client->socketSend(*childSocketId, clientPayload),
            "accepted socket should send payload bytes to the peer");

        const auto receivedResponse = client->socketRecv(*childSocketId, static_cast<uint32_t>(response.size()));
        require(receivedResponse.has_value(), "accepted socket should receive response bytes from the peer");
        require(*receivedResponse == response, "accepted socket should receive the expected response bytes");

        require(client->socketClose(*childSocketId), "client should close the accepted child socket");
        require(client->socketClose(*listenSocketId), "client should close the listening socket");
        peer.wait();

        require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its listen/accept test connection");
        require(client->removeConnection(clientConnectionId),
            "client should remove its listen/accept test connection");

        serverTransport->stop();
    }

    void testClientReceivesAsyncAcceptedTcpConnectionThroughResourceService() {
        auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

        const std::string endpoint = findListeningEndpoint(serverTransport);
        const std::string transportSpec = std::string("tcp:") + endpoint;
        const std::string listenerEndpoint = findAvailableEndpoint(35400, 35500);

        auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for async accept test");
        require(client->ping(resourceServiceNodeId),
            "client should ping the resource service before async accept test");

        const auto listenSocketId = client->listenTCP(resourceServiceNodeId, listenerEndpoint, 0, true);
        require(listenSocketId.has_value(), "client should receive an async listening socket id");

        const auto acceptReply = client->acceptTCPEx(*listenSocketId, std::nullopt, 10);
        require(acceptReply.has_value(), "accept on an async listener should receive a reply");
        require(acceptReply->error() == rsp::proto::ASYNC_SOCKET,
            "accept on an async listener should return ASYNC_SOCKET");

        TestSocketClientPeer peer;
        const std::string greeting = "async-accept-greeting";
        const std::string clientPayload = "async-accept-payload";
        const std::string response = "async-accept-response";
        peer.start(listenerEndpoint, greeting, clientPayload, response);

        rsp::proto::SocketReply newConnectionReply;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool receivedNewConnection = false;
        while (std::chrono::steady_clock::now() < deadline) {
        if (!client->tryDequeueSocketReply(newConnectionReply)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (newConnectionReply.error() == rsp::proto::NEW_CONNECTION &&
            newConnectionReply.has_socket_id() &&
            newConnectionReply.has_new_socket_id()) {
            const auto replyListenSocketId = fromProtoSocketId(newConnectionReply.socket_id());
            if (replyListenSocketId.has_value() && *replyListenSocketId == *listenSocketId) {
            receivedNewConnection = true;
            break;
            }
        }
        }

        require(receivedNewConnection, "client should receive a NEW_CONNECTION reply for async accept");
        const auto childSocketId = fromProtoSocketId(newConnectionReply.new_socket_id());
        require(childSocketId.has_value(), "NEW_CONNECTION reply should include a child socket id");

        const auto receivedGreeting = client->socketRecv(*childSocketId, static_cast<uint32_t>(greeting.size()));
        require(receivedGreeting.has_value(), "async accepted socket should receive greeting bytes from the peer");
        require(*receivedGreeting == greeting, "async accepted socket should receive the expected greeting bytes");

        require(client->socketSend(*childSocketId, clientPayload),
            "async accepted socket should send payload bytes to the peer");

        const auto receivedResponse = client->socketRecv(*childSocketId, static_cast<uint32_t>(response.size()));
        require(receivedResponse.has_value(), "async accepted socket should receive response bytes from the peer");
        require(*receivedResponse == response, "async accepted socket should receive the expected response bytes");

        require(client->socketClose(*childSocketId), "client should close the async accepted child socket");
        require(client->socketClose(*listenSocketId), "client should close the async listening socket");
        peer.wait();

        require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its async accept test connection");
        require(client->removeConnection(clientConnectionId),
            "client should remove its async accept test connection");

        serverTransport->stop();
    }

    void testSocketOwnershipByNodeIdThroughResourceService() {
        auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

        const std::string endpoint = findListeningEndpoint(serverTransport);
        const std::string transportSpec = std::string("tcp:") + endpoint;

        auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
        auto ownerClient = rsp::client::RSPClient::create();
        auto otherClient = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
        const auto ownerConnectionId = ownerClient->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
        const auto otherConnectionId = otherClient->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 3; }),
            "resource manager should authenticate the resource service and both clients");
        require(ownerClient->ping(resourceServiceNodeId),
            "owner client should ping the resource service before the socket ownership test");
        require(otherClient->ping(resourceServiceNodeId),
            "second client should ping the resource service before the socket ownership test");

        TestSocketServer exclusiveSocketServer;
        const std::string exclusiveGreeting = "exclusive-greeting";
        const std::string exclusivePayload = "exclusive-payload";
        const std::string exclusiveResponse = "exclusive-response";
        exclusiveSocketServer.start(exclusiveGreeting, exclusivePayload, exclusiveResponse);

        const auto exclusiveSocketId = ownerClient->connectTCP(resourceServiceNodeId, exclusiveSocketServer.endpoint());
        require(exclusiveSocketId.has_value(), "owner client should receive an exclusive socket id");
        otherClient->registerSocketRoute(*exclusiveSocketId, resourceServiceNodeId);

        const auto mismatchReply = otherClient->socketRecvEx(*exclusiveSocketId, 64);
        require(mismatchReply.has_value(), "second client should receive a reply for exclusive socket recv");
        require(mismatchReply->error() == rsp::proto::NODEID_MISMATCH,
            "exclusive socket recv from a different node id should return NODEID_MISMATCH");

        const auto ownerGreeting = ownerClient->socketRecv(*exclusiveSocketId, static_cast<uint32_t>(exclusiveGreeting.size()));
        require(ownerGreeting.has_value(), "owner client should still be able to receive from its exclusive socket");
        require(*ownerGreeting == exclusiveGreeting,
            "owner client should receive the expected exclusive socket greeting");
        require(ownerClient->socketSend(*exclusiveSocketId, exclusivePayload),
            "owner client should still be able to send on its exclusive socket");
        const auto ownerResponse = ownerClient->socketRecv(*exclusiveSocketId, static_cast<uint32_t>(exclusiveResponse.size()));
        require(ownerResponse.has_value(), "owner client should still be able to receive the exclusive socket response");
        require(*ownerResponse == exclusiveResponse,
            "owner client should receive the expected exclusive socket response");
        require(ownerClient->socketClose(*exclusiveSocketId), "owner client should close its exclusive socket");
        exclusiveSocketServer.wait();

        TestSocketServer sharedSocketServer;
        const std::string sharedGreeting = "shared-greeting";
        const std::string sharedPayload = "shared-payload";
        const std::string sharedResponse = "shared-response";
        sharedSocketServer.start(sharedGreeting, sharedPayload, sharedResponse);

        const auto sharedSocketId = ownerClient->connectTCP(resourceServiceNodeId, sharedSocketServer.endpoint(), 0, 0, 0, false, true);
        require(sharedSocketId.has_value(), "owner client should receive a shared socket id");
        otherClient->registerSocketRoute(*sharedSocketId, resourceServiceNodeId);

        const auto sharedGreetingReply = otherClient->socketRecvEx(*sharedSocketId, 64);
        require(sharedGreetingReply.has_value(), "second client should receive a reply for shared socket recv");
        require(sharedGreetingReply->error() == rsp::proto::SOCKET_DATA,
            "shared socket recv from a different node id should succeed");
        require(sharedGreetingReply->has_data() && sharedGreetingReply->data() == sharedGreeting,
            "second client should receive the shared socket greeting");
        require(otherClient->socketSend(*sharedSocketId, sharedPayload),
            "second client should be able to send on a shared socket");
        const auto sharedResponseReply = otherClient->socketRecvEx(*sharedSocketId, 64);
        require(sharedResponseReply.has_value(), "second client should receive the shared socket response");
        require(sharedResponseReply->error() == rsp::proto::SOCKET_DATA,
            "shared socket response should succeed for a different node id");
        require(sharedResponseReply->has_data() && sharedResponseReply->data() == sharedResponse,
            "second client should receive the shared socket response");
        require(otherClient->socketClose(*sharedSocketId), "second client should be able to close a shared socket");
        sharedSocketServer.wait();

        require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its ownership test connection");
        require(ownerClient->removeConnection(ownerConnectionId),
            "owner client should remove its ownership test connection");
        require(otherClient->removeConnection(otherConnectionId),
            "second client should remove its ownership test connection");

        serverTransport->stop();
    }

        void testSharedSocketRejectsUnsupportedOptions() {
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

            require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
                "resource manager should authenticate both endpoints for shared socket option validation");
            require(client->ping(resourceServiceNodeId),
                "client should ping the resource service before shared socket option validation");

            const auto sharedAsyncReply = client->connectTCPEx(resourceServiceNodeId, "127.0.0.1:9", 0, 0, 0, true, true);
            require(sharedAsyncReply.has_value(),
                "share_socket combined with async_data should receive a reply");
            require(sharedAsyncReply->error() == rsp::proto::INVALID_FLAGS,
                "share_socket combined with async_data should return INVALID_FLAGS");

            const auto sharedUseSocketReply = client->connectTCPEx(resourceServiceNodeId, "127.0.0.1:9", 0, 0, 0, false, true, true);
            require(sharedUseSocketReply.has_value(),
                "share_socket combined with use_socket should receive a reply");
            require(sharedUseSocketReply->error() == rsp::proto::INVALID_FLAGS,
                "share_socket combined with use_socket should return INVALID_FLAGS");

            require(resourceService->removeConnection(resourceServiceConnectionId),
                "resource service should remove its shared option validation connection");
            require(client->removeConnection(clientConnectionId),
                "client should remove its shared option validation connection");

            serverTransport->stop();
        }

            void testListeningSocketRejectsUnsupportedOptions() {
            auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
            TestResourceManager resourceManager({serverTransport});

            rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
            const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

            const std::string endpoint = findListeningEndpoint(serverTransport);
            const std::string transportSpec = std::string("tcp:") + endpoint;
            const std::string listenerEndpoint = findAvailableEndpoint(35500, 35600);

            auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
            auto client = rsp::client::RSPClient::create();

            const auto resourceServiceConnectionId =
                resourceService->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
            const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

            require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
                "resource manager should authenticate both endpoints for listening option validation");
            require(client->ping(resourceServiceNodeId),
                "client should ping the resource service before listening option validation");

            const auto sharedChildrenReply =
                client->listenTCPEx(resourceServiceNodeId, listenerEndpoint, 0, false, false, true);
            require(sharedChildrenReply.has_value(),
                "share_child_sockets without async_accept should receive a reply");
            require(sharedChildrenReply->error() == rsp::proto::INVALID_FLAGS,
                "share_child_sockets without async_accept should return INVALID_FLAGS");

            const auto asyncChildrenReply =
                client->listenTCPEx(resourceServiceNodeId, listenerEndpoint, 0, false, false, false, false, true);
            require(asyncChildrenReply.has_value(),
                "children_async_data without async_accept should receive a reply");
            require(asyncChildrenReply->error() == rsp::proto::INVALID_FLAGS,
                "children_async_data without async_accept should return INVALID_FLAGS");

            require(resourceService->removeConnection(resourceServiceConnectionId),
                "resource service should remove its listening option validation connection");
            require(client->removeConnection(clientConnectionId),
                "client should remove its listening option validation connection");

            serverTransport->stop();
            }

}  // namespace

int main() {
    try {
        testResourceServiceConnectsToResourceManager();
        testClientExchangesTcpDataThroughResourceService();
        testClientReceivesAsyncSocketDataThroughResourceService();
        testClientExchangesTcpDataThroughNativeSocketBridge();
        testClientAcceptsTcpConnectionThroughResourceService();
        testClientReceivesAsyncAcceptedTcpConnectionThroughResourceService();
        testSocketOwnershipByNodeIdThroughResourceService();
        testSharedSocketRejectsUnsupportedOptions();
        testListeningSocketRejectsUnsupportedOptions();
        std::cout << "resource service test passed" << std::endl;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "resource service test failed: " << exception.what() << std::endl;
        return 1;
    }
}