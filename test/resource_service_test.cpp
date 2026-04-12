#define RSPCLIENT_STATIC

#include "client/cpp/rsp_client.hpp"
#include "client/cpp/rsp_client_message.hpp"
#include "resource_service/resource_service.hpp"

#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/transport/transport_memory.hpp"
#include "common/transport/transport_tcp.hpp"
#include "resource_manager/resource_manager.hpp"
#include "os/os_socket.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <atomic>
#include <set>
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

std::string normalizeAddress(const rsp::os::IPAddress& address) {
    if (address.family == rsp::os::IPAddressFamily::IPv4) {
        return std::string("4:") + std::to_string(address.ipv4);
    }

    return std::string("6:") + std::string(reinterpret_cast<const char*>(address.ipv6.data()), address.ipv6.size());
}

std::string normalizeAddress(const rsp::proto::Address& address) {
    if (!address.ipv6().empty()) {
        return std::string("6:") + address.ipv6();
    }

    return std::string("4:") + std::to_string(address.ipv4());
}

std::set<std::string> expectedAdvertisedAddresses() {
    std::set<std::string> addresses;
    for (const auto& address : rsp::os::listNonLocalAddresses()) {
        addresses.insert(normalizeAddress(address));
    }

    return addresses;
}

std::optional<rsp::NodeID> fromProtoNodeId(const rsp::proto::NodeId& nodeId) {
    if (nodeId.value().size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, nodeId.value().data(), sizeof(high));
    std::memcpy(&low, nodeId.value().data() + sizeof(high), sizeof(low));
    return rsp::NodeID(high, low);
}

rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoNodeId.set_value(value);
    return protoNodeId;
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

std::string findSocketServerEndpoint(const std::shared_ptr<rsp::transport::TcpTransport>& serverTransport) {
    if (!serverTransport->listen("127.0.0.1:0")) {
        throw std::runtime_error("failed to listen on a random port for socket server test");
    }

    return std::string("127.0.0.1:") + std::to_string(serverTransport->listenedPort());
}

std::string findAvailableEndpoint(uint16_t /*firstPort*/, uint16_t /*lastPort*/) {
    // Bind a raw listener socket to port 0 to let the OS assign a free port,
    // then immediately close it. This avoids starting an accept thread (which
    // can block on stop() on Windows when closesocket doesn't interrupt accept).
    const rsp::os::SocketHandle probe = rsp::os::createTcpListener("127.0.0.1", 0, 1);
    if (!rsp::os::isValidSocket(probe)) {
        throw std::runtime_error("failed to find an available TCP port");
    }

    const uint16_t port = rsp::os::getSocketPort(probe);
    rsp::os::closeSocket(probe);
    return std::string("127.0.0.1:") + std::to_string(port);
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

std::optional<rsp::NodeID> findTcpConnectNodeId(const rsp::proto::ResourceAdvertisement& advertisement) {
    for (const auto& record : advertisement.records()) {
        if (!record.has_tcp_connect() || !record.tcp_connect().has_node_id()) {
            continue;
        }

        const auto nodeId = fromProtoNodeId(record.tcp_connect().node_id());
        if (nodeId.has_value()) {
            return nodeId;
        }
    }

    return std::nullopt;
}

class TestSocketServer {
public:
    TestSocketServer() : transport_(std::make_shared<rsp::transport::TcpTransport>()) {
        transport_->setNewConnectionCallback([this](const rsp::transport::ConnectionHandle& connection) {
            if (connectionCaptured_.exchange(true)) {
                return;
            }
            connectionPromise_.set_value(connection);
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
    std::atomic<bool> connectionCaptured_{false};
    std::thread worker_;
    std::string endpoint_;
        std::exception_ptr workerException_;
};

class PeriodicTestSocketServer {
public:
    PeriodicTestSocketServer() : transport_(std::make_shared<rsp::transport::TcpTransport>()) {
        transport_->setNewConnectionCallback([this](const rsp::transport::ConnectionHandle& connection) {
            if (connectionCaptured_.exchange(true)) {
                return;
            }
            connectionPromise_.set_value(connection);
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
    std::atomic<bool> connectionCaptured_{false};
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
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

    std::promise<rsp::encoding::EncodingHandle> handshakePromise;
    std::future<rsp::encoding::EncodingHandle> handshakeFuture = handshakePromise.get_future();
    std::atomic<bool> handshakeCaptured{false};
    resourceManager.setNewEncodingCallback([&handshakePromise, &handshakeCaptured](const rsp::encoding::EncodingHandle& encoding) {
        if (handshakeCaptured.exchange(true)) {
            return;
        }
        handshakePromise.set_value(encoding);
    });

    const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;

    auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
    const auto connectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

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
        require(waitForCondition([&resourceManager, &resourceServiceNodeId]() {
            return resourceManager.hasResourceAdvertisement(resourceServiceNodeId);
            }),
            "resource manager should store a resource advertisement for the resource service node");
    require(resourceManager.pendingMessageCount() == 0,
            "resource manager should not expose authentication traffic through its incoming message queue");

        const auto advertisement = resourceManager.resourceAdvertisement(resourceServiceNodeId);
        require(advertisement.has_value(), "resource manager should return the stored resource advertisement");
        require(advertisement->records_size() == 2,
            "resource service should advertise TCP connect and TCP listen capabilities");

        const auto expectedAddresses = expectedAdvertisedAddresses();
        bool sawTcpConnect = false;
        bool sawTcpListen = false;
        for (const auto& record : advertisement->records()) {
        if (record.has_tcp_connect()) {
            sawTcpConnect = true;
            std::set<std::string> advertisedAddresses;
            for (const auto& address : record.tcp_connect().source_addresses()) {
            advertisedAddresses.insert(normalizeAddress(address));
            }

            require(advertisedAddresses == expectedAddresses,
                "resource manager should store all non-local addresses in the TCP connect advertisement");
        }

        if (record.has_tcp_listen()) {
            sawTcpListen = true;
            std::set<std::string> advertisedAddresses;
            for (const auto& address : record.tcp_listen().listen_address()) {
            advertisedAddresses.insert(normalizeAddress(address));
            }

            require(advertisedAddresses == expectedAddresses,
                "resource manager should store all non-local addresses in the TCP listen advertisement");
            require(record.tcp_listen().has_allowed_range(),
                "resource service should advertise an allowed listen port range");
            require(record.tcp_listen().allowed_range().start_port() == 0 &&
                record.tcp_listen().allowed_range().end_port() == 0,
                "resource service should currently advertise an unrestricted listen port range");
        }
        }

        require(sawTcpConnect, "resource service should send a TCP connect advertisement record");
        require(sawTcpListen, "resource service should send a TCP listen advertisement record");

    require(resourceService->removeConnection(connectionId), "resource service should remove an existing connection");
    require(!resourceService->hasConnections(), "resource service should have no remaining connections");

    serverTransport->stop();
}

void testClientExchangesTcpDataThroughResourceService() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

    const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;

    auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
    auto client = rsp::client::RSPClient::create();

    const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

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

void testClientDiscoversResourceServiceThroughResourceQuery() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

    const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;

    auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
    auto client = rsp::client::RSPClientMessage::create();

    const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate the resource service and client for discovery test");
    require(waitForCondition([&resourceManager, &resourceServiceNodeId]() {
                return resourceManager.hasResourceAdvertisement(resourceServiceNodeId);
            }),
            "resource manager should store the resource service advertisement before discovery queries run");

    rsp::proto::RSPMessage query;
    *query.mutable_source() = toProtoNodeId(client->nodeId());
    *query.mutable_destination() = toProtoNodeId(resourceManager.nodeId());
    query.mutable_resource_query();
    require(client->send(query),
            "client should send a resource query to the resource manager");

    require(waitForCondition([&client]() { return client->pendingMessageCount() == 1; }),
            "client should receive a resource advertisement reply from the resource manager");

    rsp::proto::RSPMessage reply;
    require(client->tryDequeueMessage(reply),
            "client should dequeue the resource advertisement reply");
    require(reply.has_resource_advertisement(),
            "resource query reply should contain a resource advertisement");

    const auto& advertisement = reply.resource_advertisement();
    require(advertisement.records_size() == 2,
            "resource query should return the resource service TCP connect and listen records");

    const auto expectedAddresses = expectedAdvertisedAddresses();
    bool sawTcpConnect = false;
    bool sawTcpListen = false;
    for (const auto& record : advertisement.records()) {
        if (record.has_tcp_connect()) {
            sawTcpConnect = true;
            require(record.tcp_connect().has_node_id(),
                    "resource manager should stamp TCP connect records with the owning node id");
            const auto nodeId = fromProtoNodeId(record.tcp_connect().node_id());
            require(nodeId.has_value() && *nodeId == resourceServiceNodeId,
                    "resource manager should identify the resource service on TCP connect records");

            std::set<std::string> advertisedAddresses;
            for (const auto& address : record.tcp_connect().source_addresses()) {
                advertisedAddresses.insert(normalizeAddress(address));
            }
            require(advertisedAddresses == expectedAddresses,
                    "resource query should return all advertised TCP connect addresses");
        }

        if (record.has_tcp_listen()) {
            sawTcpListen = true;
            require(record.tcp_listen().has_node_id(),
                    "resource manager should stamp TCP listen records with the owning node id");
            const auto nodeId = fromProtoNodeId(record.tcp_listen().node_id());
            require(nodeId.has_value() && *nodeId == resourceServiceNodeId,
                    "resource manager should identify the resource service on TCP listen records");

            std::set<std::string> advertisedAddresses;
            for (const auto& address : record.tcp_listen().listen_address()) {
                advertisedAddresses.insert(normalizeAddress(address));
            }
            require(advertisedAddresses == expectedAddresses,
                    "resource query should return all advertised TCP listen addresses");
            require(record.tcp_listen().has_allowed_range() &&
                        record.tcp_listen().allowed_range().start_port() == 0 &&
                        record.tcp_listen().allowed_range().end_port() == 0,
                    "resource query should preserve the unrestricted listen port range");
        }
    }

    require(sawTcpConnect, "resource query reply should include a TCP connect record");
    require(sawTcpListen, "resource query reply should include a TCP listen record");

    require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its discovery test connection");
    require(client->removeConnection(clientConnectionId),
            "client should remove its discovery test connection");

    serverTransport->stop();
}

    void testClientDiscoversTcpConnectResourceAndExchangesData() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;

        auto resourceService = rsp::resource_service::ResourceService::create();
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate the resource service and client for discovery-driven connect test");
        require(waitForCondition([&resourceManager]() { return resourceManager.resourceAdvertisementCount() == 1; }),
            "resource manager should store one discovered resource advertisement before client lookup");

        const auto resourceManagerNodeId = client->peerNodeID(clientConnectionId);
        require(resourceManagerNodeId.has_value(),
            "client should discover the resource manager node id from its authenticated connection");
        require(client->queryResources(*resourceManagerNodeId, "tcp connect"),
            "client should query the resource manager for TCP connect resources");
        require(waitForCondition([&client]() { return client->pendingResourceAdvertisementCount() == 1; }),
            "client should receive a resource advertisement response for the TCP connect query");

        rsp::proto::ResourceAdvertisement advertisement;
        require(client->tryDequeueResourceAdvertisement(advertisement),
            "client should dequeue the discovered TCP connect advertisement");
        const auto discoveredResourceServiceNodeId = findTcpConnectNodeId(advertisement);
        require(discoveredResourceServiceNodeId.has_value(),
            "client should discover a resource service node id from a TCP connect advertisement record");

        TestSocketServer socketServer;
        const std::string greeting = "discover-connect-greeting";
        const std::string clientPayload = "discover-connect-payload";
        const std::string serverResponse = "discover-connect-response";
        socketServer.start(greeting, clientPayload, serverResponse);

        const auto socketId = client->connectTCP(*discoveredResourceServiceNodeId, socketServer.endpoint());
        require(socketId.has_value(),
            "client should connect through the discovered TCP connect resource without test-supplied node ids");

        const auto receivedGreeting = client->socketRecv(*socketId, static_cast<uint32_t>(greeting.size()));
        require(receivedGreeting.has_value(),
            "client should receive greeting bytes from the TCP server through the discovered resource service");
        require(*receivedGreeting == greeting,
            "client should receive the expected greeting bytes through the discovered resource service");

        require(client->socketSend(*socketId, clientPayload),
            "client should send payload bytes through the discovered resource service");

        const auto receivedResponse = client->socketRecv(*socketId, static_cast<uint32_t>(serverResponse.size()));
        require(receivedResponse.has_value(),
            "client should receive response bytes from the TCP server through the discovered resource service");
        require(*receivedResponse == serverResponse,
            "client should receive the expected response bytes through the discovered resource service");

        require(client->socketClose(*socketId),
            "client should close the discovered TCP connection through the resource service");
        socketServer.wait();

        require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its discovery-driven connect test connection");
        require(client->removeConnection(clientConnectionId),
            "client should remove its discovery-driven connect test connection");

        serverTransport->stop();
    }

    void testEmptyResourceAdvertisementClearsStoredResourceServiceRecords() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

        const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;

        auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
        const auto connectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 1; }),
            "resource manager should authenticate the resource service before clearing advertisements");
        require(waitForCondition([&resourceManager, &resourceServiceNodeId]() {
            return resourceManager.hasResourceAdvertisement(resourceServiceNodeId);
            }),
            "resource manager should store the initial resource service advertisement");

        rsp::proto::RSPMessage message;
        *message.mutable_source() = toProtoNodeId(resourceServiceNodeId);
        *message.mutable_destination() = toProtoNodeId(resourceManager.nodeId());
        message.mutable_resource_advertisement();
        require(resourceService->send(message),
            "resource service should send an empty resource advertisement to clear stored records");

        require(waitForCondition([&resourceManager, &resourceServiceNodeId]() {
            return !resourceManager.hasResourceAdvertisement(resourceServiceNodeId);
            }),
            "resource manager should erase stored advertisements for a node that sends an empty advertisement");

        require(resourceService->removeConnection(connectionId),
            "resource service should remove its empty-advertisement test connection");
        serverTransport->stop();
    }

    void testFailedRouteClearsStoredResourceServiceRecords() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        const rsp::NodeID unroutableNodeId{rsp::GUID()};

        const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
        require(serverTransport->listen(memoryChannel), "memory transport listener should start");

        rsp::proto::RSPMessage advertisementMessage;
        *advertisementMessage.mutable_source() = toProtoNodeId(unroutableNodeId);
        *advertisementMessage.mutable_destination() = toProtoNodeId(resourceManager.nodeId());
        advertisementMessage.mutable_resource_advertisement()->add_records()->mutable_tcp_connect();
        require(resourceManager.enqueueInput(advertisementMessage),
            "resource manager should accept a directly injected resource advertisement for route cleanup testing");
        require(waitForCondition([&resourceManager, &unroutableNodeId]() {
            return resourceManager.hasResourceAdvertisement(unroutableNodeId);
            }),
            "resource manager should store advertisements even when the advertised node id is not routable");

        rsp::proto::RSPMessage message;
        *message.mutable_source() = toProtoNodeId(resourceManager.nodeId());
        *message.mutable_destination() = toProtoNodeId(unroutableNodeId);
        message.mutable_ping_request()->mutable_nonce()->set_value("route-failure");
        message.mutable_ping_request()->set_sequence(1);
        require(!resourceManager.routeAndSend(message),
            "routeAndSend should fail when no active route exists for the destination node id");
        require(!resourceManager.hasResourceAdvertisement(unroutableNodeId),
            "routeAndSend failure should erase stored advertisements for the destination node");
        serverTransport->stop();
    }

void testClientReceivesAsyncSocketDataThroughResourceService() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

    const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;

    auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
    auto client = rsp::client::RSPClient::create();

    const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

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
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

    const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;

    auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
    auto client = rsp::client::RSPClient::create();

    const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

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
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

        const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;
        const std::string listenerEndpoint = findAvailableEndpoint(35300, 35400);

        auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

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
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

        const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;
        const std::string listenerEndpoint = findAvailableEndpoint(35400, 35500);

        auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

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

        void testClientAcceptsTcpConnectionThroughNativeSocketBridge() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

        const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;
        const std::string listenerEndpoint = findAvailableEndpoint(35500, 35600);

        auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
            resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for native accept bridge test");
        require(client->ping(resourceServiceNodeId),
            "client should ping the resource service before native accept bridge test");

        const auto listenSocketId = client->listenTCP(resourceServiceNodeId, listenerEndpoint);
        require(listenSocketId.has_value(), "client should receive a listening socket id for native accept bridge test");

        TestSocketClientPeer peer;
        const std::string greeting = "native-accept-greeting";
        const std::string clientPayload = "native-accept-payload";
        const std::string response = "native-accept-response";
        peer.start(listenerEndpoint, greeting, clientPayload, response);

        const auto localSocket = client->acceptTCPSocket(*listenSocketId, std::nullopt, 5000);
        require(localSocket.has_value(), "client should receive a local native socket for accepted TCP connections");

        const auto receivedGreeting = recvExactSocket(*localSocket, greeting.size());
        require(receivedGreeting.has_value(), "accepted native socket should receive greeting bytes from the peer");
        require(*receivedGreeting == greeting, "accepted native socket should receive the expected greeting bytes");

        require(sendAllSocket(*localSocket, clientPayload),
            "accepted native socket should send payload bytes to the peer");

        const auto receivedResponse = recvExactSocket(*localSocket, response.size());
        require(receivedResponse.has_value(), "accepted native socket should receive response bytes from the peer");
        require(*receivedResponse == response, "accepted native socket should receive the expected response bytes");

        rsp::os::closeSocket(*localSocket);
        require(client->socketClose(*listenSocketId), "client should close the listening socket after native accept bridge test");
        peer.wait();

        require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its native accept bridge test connection");
        require(client->removeConnection(clientConnectionId),
            "client should remove its native accept bridge test connection");

        serverTransport->stop();
        }

    void testSocketOwnershipByNodeIdThroughResourceService() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

        const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;

        auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
        auto ownerClient = rsp::client::RSPClient::create();
        auto otherClient = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto ownerConnectionId = ownerClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto otherConnectionId = otherClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

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

        void testClientListensWithNativeSocketBridgeAndAcceptsBidirectionalTraffic() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

        const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;
        const std::string listenerEndpoint = findAvailableEndpoint(35600, 35700);

        auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
            resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for native listen bridge test");
        require(client->ping(resourceServiceNodeId),
            "client should ping the resource service before native listen bridge test");

        const auto localListener = client->listenTCPSocket(resourceServiceNodeId, listenerEndpoint);
        require(localListener.has_value(), "client should receive a local listener socket for the native listen bridge");

        TestSocketClientPeer peer;
        const std::string greeting = "native-listen-greeting";
        const std::string clientPayload = "native-listen-payload";
        const std::string response = "native-listen-response";
        peer.start(listenerEndpoint, greeting, clientPayload, response);

        const auto acceptedSocket = rsp::os::acceptSocket(*localListener);
        require(rsp::os::isValidSocket(acceptedSocket),
            "client should accept a local socket from the native listen bridge");

        const auto receivedGreeting = recvExactSocket(acceptedSocket, greeting.size());
        require(receivedGreeting.has_value(),
            "accepted local socket should receive greeting bytes from the remote peer");
        require(*receivedGreeting == greeting,
            "accepted local socket should receive the expected greeting bytes");

        require(sendAllSocket(acceptedSocket, clientPayload),
            "accepted local socket should send payload bytes to the remote peer");

        const auto receivedResponse = recvExactSocket(acceptedSocket, response.size());
        require(receivedResponse.has_value(),
            "accepted local socket should receive response bytes from the remote peer");
        require(*receivedResponse == response,
            "accepted local socket should receive the expected response bytes");

        rsp::os::closeSocket(acceptedSocket);
        rsp::os::closeSocket(*localListener);
        peer.wait();

        require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its native listen bridge test connection");
        require(client->removeConnection(clientConnectionId),
            "client should remove its native listen bridge test connection");

        serverTransport->stop();
        }

        void testSharedSocketRejectsUnsupportedOptions() {
            auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
            TestResourceManager resourceManager({serverTransport});

            rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
            const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

            const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;

            auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
            auto client = rsp::client::RSPClient::create();

            const auto resourceServiceConnectionId =
            resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
            const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

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
            auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
            TestResourceManager resourceManager({serverTransport});

            rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
            const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

            const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(memoryChannel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + memoryChannel;
            const std::string listenerEndpoint = findAvailableEndpoint(35500, 35600);

            auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
            auto client = rsp::client::RSPClient::create();

            const auto resourceServiceConnectionId =
                resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
            const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

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
        testEmptyResourceAdvertisementClearsStoredResourceServiceRecords();
        testFailedRouteClearsStoredResourceServiceRecords();
        testClientDiscoversResourceServiceThroughResourceQuery();
        testClientDiscoversTcpConnectResourceAndExchangesData();
        testClientExchangesTcpDataThroughResourceService();
        testClientReceivesAsyncSocketDataThroughResourceService();
        testClientExchangesTcpDataThroughNativeSocketBridge();
        testClientAcceptsTcpConnectionThroughResourceService();
        testClientReceivesAsyncAcceptedTcpConnectionThroughResourceService();
        testClientAcceptsTcpConnectionThroughNativeSocketBridge();
        testClientListensWithNativeSocketBridgeAndAcceptsBidirectionalTraffic();
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
