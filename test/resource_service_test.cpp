#define RSPCLIENT_STATIC

#include "client/cpp/rsp_client.hpp"
#include "client/cpp/rsp_client_message.hpp"
#include "common/service_message.hpp"
#include "logging/logging.pb.h"
#include "name_service/name_service.hpp"
#include "resource_service/bsd_sockets/resource_service_bsd_sockets.hpp"
#include "resource_service/bsd_sockets/bsd_sockets_logging.pb.h"

#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/transport/transport_memory.hpp"
#include "common/transport/transport_tcp.hpp"
#include "resource_manager/resource_manager.hpp"
#include "resource_manager/schema_registry.hpp"
#include "os/os_socket.hpp"

#include <chrono>
#include <deque>
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

std::optional<rsp::GUID> fromProtoStreamId(const rsp::proto::StreamID& socketId) {
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

std::optional<rsp::NodeID> findServiceNodeIdByProto(const rsp::proto::ResourceQueryReply& reply,
                                                    const std::string& protoFileName) {
    for (const auto& service : reply.services()) {
        if (!service.has_node_id() || !service.has_schema() ||
            service.schema().proto_file_name() != protoFileName) {
            continue;
        }

        const auto nodeId = fromProtoNodeId(service.node_id());
        if (nodeId.has_value()) {
            return nodeId;
        }
    }

    return std::nullopt;
}

std::optional<rsp::NodeID> findServiceNodeIdByProto(const rsp::client::ResourceQueryResult& reply,
                                                    const std::string& protoFileName) {
    for (const auto& service : reply.services) {
        if (service.protoFileName != protoFileName) {
            continue;
        }
        if (service.nodeId == rsp::NodeID{}) {
            continue;
        }
        return service.nodeId;
    }
    return std::nullopt;
}

rsp::proto::LogASTFieldPath makeFieldPath(std::initializer_list<std::string> segments) {
    rsp::proto::LogASTFieldPath path;
    for (const auto& segment : segments) {
        path.add_segments(segment);
    }
    return path;
}

rsp::proto::LogASTMessageTree makeFieldEqualsString(std::initializer_list<std::string> segments,
                                                    const std::string& value) {
    rsp::proto::LogASTMessageTree tree;
    *tree.mutable_field_equals()->mutable_path() = makeFieldPath(segments);
    tree.mutable_field_equals()->mutable_value()->set_string_value(value);
    return tree;
}

rsp::proto::LogASTMessageTree makeFieldEqualsUint(std::initializer_list<std::string> segments,
                                                  uint64_t value) {
    rsp::proto::LogASTMessageTree tree;
    *tree.mutable_field_equals()->mutable_path() = makeFieldPath(segments);
    tree.mutable_field_equals()->mutable_value()->set_uint_value(value);
    return tree;
}

uint32_t parseEndpointPort(const std::string& endpoint) {
    const auto separator = endpoint.rfind(':');
    if (separator == std::string::npos || separator + 1 >= endpoint.size()) {
        throw std::runtime_error("endpoint is missing a port: " + endpoint);
    }

    return static_cast<uint32_t>(std::stoul(endpoint.substr(separator + 1)));
}

class LoggingTestClient : public rsp::client::full::RSPClient {
public:
    using Ptr = std::shared_ptr<LoggingTestClient>;

    static Ptr create() {
        return Ptr(new LoggingTestClient(rsp::KeyPair::generateP256()));
    }

    bool queryResources(const rsp::NodeID& nodeId, const std::string& query = std::string()) {
        rsp::proto::RSPMessage message;
        *message.mutable_destination() = toProtoNodeId(nodeId);
        auto* resourceQuery = message.mutable_resource_query();
        if (!query.empty()) {
            resourceQuery->set_query(query);
        }
        return this->send(message);
    }

    bool querySchemas(const rsp::NodeID& nodeId,
                      const std::string& protoFileName = std::string(),
                      const std::string& schemaHash = std::string()) {
        rsp::proto::RSPMessage message;
        *message.mutable_destination() = toProtoNodeId(nodeId);
        auto* schemaRequest = message.mutable_schema_request();
        if (!protoFileName.empty()) {
            schemaRequest->set_proto_file_name(protoFileName);
        }
        if (!schemaHash.empty()) {
            schemaRequest->set_schema_hash(schemaHash);
        }
        return this->send(message);
    }

    bool subscribeToLogs(const rsp::NodeID& nodeId, const rsp::proto::LogSubscribeRequest& request) {
        rsp::proto::RSPMessage message;
        *message.mutable_destination() = toProtoNodeId(nodeId);
        *message.mutable_log_subscribe_request() = request;
        return this->send(message);
    }

    bool unsubscribeFromLogs(const rsp::NodeID& nodeId, const rsp::proto::LogUnsubscribeRequest& request) {
        rsp::proto::RSPMessage message;
        *message.mutable_destination() = toProtoNodeId(nodeId);
        *message.mutable_log_unsubscribe_request() = request;
        return this->send(message);
    }

    bool tryDequeueSubscribeReply(rsp::proto::LogSubscribeReply& reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (subscribeReplies_.empty()) {
            return false;
        }

        reply = subscribeReplies_.front();
        subscribeReplies_.pop_front();
        return true;
    }

    bool tryDequeueUnsubscribeReply(rsp::proto::LogUnsubscribeReply& reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (unsubscribeReplies_.empty()) {
            return false;
        }

        reply = unsubscribeReplies_.front();
        unsubscribeReplies_.pop_front();
        return true;
    }

    bool tryDequeueLogRecord(rsp::proto::LogRecord& record) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (records_.empty()) {
            return false;
        }

        record = records_.front();
        records_.pop_front();
        return true;
    }

    std::size_t pendingLogRecordCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }

    std::size_t pendingSubscribeReplyCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return subscribeReplies_.size();
    }

    bool tryDequeueResourceQueryReply(rsp::proto::ResourceQueryReply& reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (resourceQueryReplies_.empty()) {
            return false;
        }

        reply = resourceQueryReplies_.front();
        resourceQueryReplies_.pop_front();
        return true;
    }

    std::size_t pendingResourceQueryReplyCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return resourceQueryReplies_.size();
    }

    bool tryDequeueSchemaReply(rsp::proto::SchemaReply& reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (schemaReplies_.empty()) {
            return false;
        }

        reply = schemaReplies_.front();
        schemaReplies_.pop_front();
        return true;
    }

    std::size_t pendingSchemaReplyCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return schemaReplies_.size();
    }

protected:
    explicit LoggingTestClient(rsp::KeyPair keyPair) : rsp::client::full::RSPClient(std::move(keyPair)) {}

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override {
        if (message.has_ping_reply()) {
            return true;
        }

        if (rsp::hasServiceMessage<rsp::proto::EndorsementDone>(message)) {
            return true;
        }

        if (message.has_resource_advertisement()) {
            return true;
        }

        if (message.has_resource_query_reply()) {
            std::lock_guard<std::mutex> lock(mutex_);
            resourceQueryReplies_.push_back(message.resource_query_reply());
            return true;
        }

        if (message.has_schema_reply()) {
            std::lock_guard<std::mutex> lock(mutex_);
            schemaReplies_.push_back(message.schema_reply());
            return true;
        }

        if (message.has_log_subscribe_reply()) {
            std::lock_guard<std::mutex> lock(mutex_);
            subscribeReplies_.push_back(message.log_subscribe_reply());
            return true;
        }

        if (message.has_log_unsubscribe_reply()) {
            std::lock_guard<std::mutex> lock(mutex_);
            unsubscribeReplies_.push_back(message.log_unsubscribe_reply());
            return true;
        }

        if (message.has_log_record()) {
            std::lock_guard<std::mutex> lock(mutex_);
            records_.push_back(message.log_record());
            return true;
        }

        if (message.has_error()) {
            return true;
        }

        return rsp::client::full::RSPClient::handleNodeSpecificMessage(message);
    }

private:
    mutable std::mutex mutex_;
    std::deque<rsp::proto::ResourceQueryReply> resourceQueryReplies_;
    std::deque<rsp::proto::SchemaReply> schemaReplies_;
    std::deque<rsp::proto::LogSubscribeReply> subscribeReplies_;
    std::deque<rsp::proto::LogUnsubscribeReply> unsubscribeReplies_;
    std::deque<rsp::proto::LogRecord> records_;
};

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

    auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
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

    auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
    auto client = rsp::client::RSPClient::create();

    const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

    require(resourceService->hasConnection(resourceServiceConnectionId),
            "resource service should stay connected to the resource manager");
    require(client->hasConnection(clientConnectionId),
            "client should stay connected to the resource manager");
    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both the client and resource service");

    TestSocketServer socketServer;
    const std::string greeting = "server-greeting";
    const std::string clientPayload = "client-payload";
    const std::string serverResponse = "server-response";
    socketServer.start(greeting, clientPayload, serverResponse);

    const auto socketId = client->connectTCP(resourceServiceNodeId, socketServer.endpoint());
    require(socketId.has_value(), "client should receive a socket id from the resource service");

    const auto receivedGreeting = client->streamRecv(*socketId, static_cast<uint32_t>(greeting.size()));
    require(receivedGreeting.has_value(), "client should receive greeting bytes from the remote TCP server");
    require(*receivedGreeting == greeting, "client should receive the expected greeting bytes");

    require(client->streamSend(*socketId, clientPayload),
            "client should send payload bytes to the remote TCP server through the resource service");

    const auto receivedResponse = client->streamRecv(*socketId, static_cast<uint32_t>(serverResponse.size()));
    require(receivedResponse.has_value(), "client should receive response bytes from the remote TCP server");
    require(*receivedResponse == serverResponse, "client should receive the expected response bytes");

    require(client->streamClose(*socketId), "client should close the remote TCP socket through the resource service");
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

    auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
    auto client = rsp::client::RSPClientMessage::create();

    const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

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
        require(reply.has_resource_query_reply(),
            "resource query reply should contain discovered services");

        const auto& queryReply = reply.resource_query_reply();
        require(queryReply.services_size() >= 2,
            "resource query should return both advertised service schemas for the resource service node");
        const auto bsdSocketsNodeId = findServiceNodeIdByProto(queryReply, "bsd_sockets.proto");
        require(bsdSocketsNodeId.has_value() && *bsdSocketsNodeId == resourceServiceNodeId,
            "resource query should identify the bsd_sockets schema on the resource service node");
        const auto loggingNodeId = findServiceNodeIdByProto(
            queryReply, "resource_service/bsd_sockets/bsd_sockets_logging.proto");
        require(loggingNodeId.has_value() && *loggingNodeId == resourceServiceNodeId,
            "resource query should identify the logging schema on the same resource service node");

        bool sawAcceptedTypeUrls = false;
        for (const auto& service : queryReply.services()) {
            if (!service.has_schema() || service.schema().proto_file_name() != "bsd_sockets.proto") {
                continue;
            }

            sawAcceptedTypeUrls = service.schema().accepted_type_urls_size() >= 2;
            break;
        }
        require(sawAcceptedTypeUrls,
            "resource query should preserve accepted type urls for the bsd_sockets discovered service schema");

    require(resourceService->removeConnection(resourceServiceConnectionId),
            "resource service should remove its discovery test connection");
    require(client->removeConnection(clientConnectionId),
            "client should remove its discovery test connection");

    serverTransport->stop();
}

    void testClientDiscoversNameServiceThroughResourceQuery() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair nameServiceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID nameServiceNodeId = nameServiceKeyPair.nodeID();

        const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
        require(serverTransport->listen(memoryChannel), "memory transport listener should start");
        const std::string transportSpec = "memory:" + memoryChannel;

        auto nameService = rsp::name_service::NameService::create(std::move(nameServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto nameServiceConnectionId =
        nameService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate the name service and client for discovery test");
        require(waitForCondition([&resourceManager, &nameServiceNodeId]() {
            return resourceManager.hasResourceAdvertisement(nameServiceNodeId);
            }),
            "resource manager should store the name service schema-only advertisement");

        const auto resourceManagerNodeId = client->peerNodeID(clientConnectionId);
        require(resourceManagerNodeId.has_value(),
            "client should discover the resource manager node id from its authenticated connection");
        require(client->queryResources(*resourceManagerNodeId,
                       "service.proto_file_name = \"name_service.proto\""),
            "client should query the resource manager for name service schemas");
        require(waitForCondition([&client]() { return client->pendingResourceQueryReplyCount() == 1; }),
            "client should receive a resource query reply for the name service query");

        rsp::client::ResourceQueryResult queryReply;
        require(client->tryDequeueResourceQueryReply(queryReply),
            "client should dequeue the discovered name service reply");
        require(queryReply.services.size() == 1,
            "name service query should return one matching discovered service");
        const auto discoveredNameServiceNodeId = findServiceNodeIdByProto(queryReply, "name_service.proto");
        require(discoveredNameServiceNodeId.has_value() && *discoveredNameServiceNodeId == nameServiceNodeId,
            "name service query should identify the name service node");

        require(nameService->removeConnection(nameServiceConnectionId),
            "name service should remove its discovery test connection");
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

        auto resourceService = rsp::resource_service::BsdSocketsResourceService::create();
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate the resource service and client for discovery-driven connect test");
        require(waitForCondition([&resourceManager]() { return resourceManager.resourceAdvertisementCount() == 1; }),
            "resource manager should store one discovered resource advertisement before client lookup");

        const auto resourceManagerNodeId = client->peerNodeID(clientConnectionId);
        require(resourceManagerNodeId.has_value(),
            "client should discover the resource manager node id from its authenticated connection");
        require(client->queryResources(*resourceManagerNodeId,
                                       "service.accepted_type_urls HAS \"type.rsp/rsp.proto.ConnectTCPRequest\""),
            "client should query the resource manager for TCP connect-capable services");
        require(waitForCondition([&client]() { return client->pendingResourceQueryReplyCount() == 1; }),
            "client should receive a resource query response for the TCP connect query");

        rsp::client::ResourceQueryResult queryReply;
        require(client->tryDequeueResourceQueryReply(queryReply),
            "client should dequeue the discovered TCP connect service reply");
        const auto discoveredResourceServiceNodeId = findServiceNodeIdByProto(queryReply, "bsd_sockets.proto");
        require(discoveredResourceServiceNodeId.has_value(),
            "client should discover a resource service node id from a schema-based query reply");

        TestSocketServer socketServer;
        const std::string greeting = "discover-connect-greeting";
        const std::string clientPayload = "discover-connect-payload";
        const std::string serverResponse = "discover-connect-response";
        socketServer.start(greeting, clientPayload, serverResponse);

        const auto socketId = client->connectTCP(*discoveredResourceServiceNodeId, socketServer.endpoint());
        require(socketId.has_value(),
            "client should connect through the discovered TCP connect resource without test-supplied node ids");

        const auto receivedGreeting = client->streamRecv(*socketId, static_cast<uint32_t>(greeting.size()));
        require(receivedGreeting.has_value(),
            "client should receive greeting bytes from the TCP server through the discovered resource service");
        require(*receivedGreeting == greeting,
            "client should receive the expected greeting bytes through the discovered resource service");

        require(client->streamSend(*socketId, clientPayload),
            "client should send payload bytes through the discovered resource service");

        const auto receivedResponse = client->streamRecv(*socketId, static_cast<uint32_t>(serverResponse.size()));
        require(receivedResponse.has_value(),
            "client should receive response bytes from the TCP server through the discovered resource service");
        require(*receivedResponse == serverResponse,
            "client should receive the expected response bytes through the discovered resource service");

        require(client->streamClose(*socketId),
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

        auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
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

    auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
    auto client = rsp::client::RSPClient::create();

    const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for async socket test");
    PeriodicTestSocketServer socketServer;
    const std::vector<std::string> periodicMessages = {"tick-1", "tick-2", "tick-3"};
    std::string expectedStream;
    for (const auto& message : periodicMessages) {
        expectedStream += message;
    }
    socketServer.start(periodicMessages, 100);

    const auto socketId = client->connectTCP(resourceServiceNodeId, socketServer.endpoint(), 0, 0, 0, true);
    require(socketId.has_value(), "client should receive a socket id for async socket connection");

    bool sawAsyncStreamReply = false;
    std::string receivedStream;
    const auto recvReply = client->streamRecvEx(*socketId, 32);
    require(recvReply.has_value(), "client should receive a socket reply when socket_recv is used on an async socket");
    if (recvReply->status == rsp::client::StreamStatus::Async) {
        sawAsyncStreamReply = true;
    } else if (recvReply->status == rsp::client::StreamStatus::Data && !recvReply->data.empty()) {
        receivedStream += recvReply->data;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!sawAsyncStreamReply || receivedStream.size() < expectedStream.size()) &&
           std::chrono::steady_clock::now() < deadline) {
        rsp::client::StreamResult reply;
        if (client->tryDequeueStreamResult(reply)) {
            if (reply.status == rsp::client::StreamStatus::Async) {
                sawAsyncStreamReply = true;
            } else if (reply.status == rsp::client::StreamStatus::Data && !reply.data.empty()) {
                receivedStream += reply.data;
            }
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    require(sawAsyncStreamReply,
            "socket_recv on an async socket should eventually reply with ASYNC_STREAM");
    require(receivedStream == expectedStream,
        "client should receive the expected periodic async socket payload stream in order");

    require(client->streamClose(*socketId), "client should close the async socket through the resource service");
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

    auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
    auto client = rsp::client::RSPClient::create();

    const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for native socket bridge test");
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

        auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for listen/accept test");
        const auto listenSocketId = client->listenTCP(resourceServiceNodeId, listenerEndpoint);
        require(listenSocketId.has_value(), "client should receive a listening socket id from the resource service");

        TestSocketClientPeer peer;
        const std::string greeting = "accept-greeting";
        const std::string clientPayload = "accept-payload";
        const std::string response = "accept-response";
        peer.start(listenerEndpoint, greeting, clientPayload, response);

        const auto childSocketId = client->acceptTCP(*listenSocketId, std::nullopt, 5000);
        require(childSocketId.has_value(), "client should accept an inbound TCP connection through the resource service");

        const auto receivedGreeting = client->streamRecv(*childSocketId, static_cast<uint32_t>(greeting.size()));
        require(receivedGreeting.has_value(), "accepted socket should receive greeting bytes from the peer");
        require(*receivedGreeting == greeting, "accepted socket should receive the expected greeting bytes");

        require(client->streamSend(*childSocketId, clientPayload),
            "accepted socket should send payload bytes to the peer");

        const auto receivedResponse = client->streamRecv(*childSocketId, static_cast<uint32_t>(response.size()));
        require(receivedResponse.has_value(), "accepted socket should receive response bytes from the peer");
        require(*receivedResponse == response, "accepted socket should receive the expected response bytes");

        require(client->streamClose(*childSocketId), "client should close the accepted child socket");
        require(client->streamClose(*listenSocketId), "client should close the listening socket");
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

        auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for async accept test");
        const auto listenSocketId = client->listenTCP(resourceServiceNodeId, listenerEndpoint, 0, true);
        require(listenSocketId.has_value(), "client should receive an async listening socket id");

        const auto acceptReply = client->acceptTCPEx(*listenSocketId, std::nullopt, 10);
        require(acceptReply.has_value(), "accept on an async listener should receive a reply");
        require(acceptReply->status == rsp::client::StreamStatus::Async,
            "accept on an async listener should return ASYNC_STREAM");

        TestSocketClientPeer peer;
        const std::string greeting = "async-accept-greeting";
        const std::string clientPayload = "async-accept-payload";
        const std::string response = "async-accept-response";
        peer.start(listenerEndpoint, greeting, clientPayload, response);

        rsp::client::StreamResult newConnectionReply;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool receivedNewConnection = false;
        while (std::chrono::steady_clock::now() < deadline) {
        if (!client->tryDequeueStreamResult(newConnectionReply)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (newConnectionReply.status == rsp::client::StreamStatus::NewConnection &&
            newConnectionReply.hasNewStreamId &&
            newConnectionReply.streamId == *listenSocketId) {
            receivedNewConnection = true;
            break;
        }
        }

        require(receivedNewConnection, "client should receive a NEW_CONNECTION reply for async accept");
        const rsp::GUID childSocketId = newConnectionReply.newStreamId;
        require(newConnectionReply.hasNewStreamId, "NEW_CONNECTION reply should include a child socket id");

        const auto receivedGreeting = client->streamRecv(childSocketId, static_cast<uint32_t>(greeting.size()));
        require(receivedGreeting.has_value(), "async accepted socket should receive greeting bytes from the peer");
        require(*receivedGreeting == greeting, "async accepted socket should receive the expected greeting bytes");

        require(client->streamSend(childSocketId, clientPayload),
            "async accepted socket should send payload bytes to the peer");

        const auto receivedResponse = client->streamRecv(childSocketId, static_cast<uint32_t>(response.size()));
        require(receivedResponse.has_value(), "async accepted socket should receive response bytes from the peer");
        require(*receivedResponse == response, "async accepted socket should receive the expected response bytes");

        require(client->streamClose(childSocketId), "client should close the async accepted child socket");
        require(client->streamClose(*listenSocketId), "client should close the async listening socket");
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

        auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
            resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for native accept bridge test");
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
        require(client->streamClose(*listenSocketId), "client should close the listening socket after native accept bridge test");
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

        auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
        auto ownerClient = rsp::client::RSPClient::create();
        auto otherClient = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
        resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto ownerConnectionId = ownerClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();
        const auto otherConnectionId = otherClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 3; }),
            "resource manager should authenticate the resource service and both clients");
        TestSocketServer exclusiveSocketServer;
        const std::string exclusiveGreeting = "exclusive-greeting";
        const std::string exclusivePayload = "exclusive-payload";
        const std::string exclusiveResponse = "exclusive-response";
        exclusiveSocketServer.start(exclusiveGreeting, exclusivePayload, exclusiveResponse);

        const auto exclusiveSocketId = ownerClient->connectTCP(resourceServiceNodeId, exclusiveSocketServer.endpoint());
        require(exclusiveSocketId.has_value(), "owner client should receive an exclusive socket id");
        otherClient->registerStreamRoute(*exclusiveSocketId, resourceServiceNodeId);

        const auto mismatchReply = otherClient->streamRecvEx(*exclusiveSocketId, 64);
        require(mismatchReply.has_value(), "second client should receive a reply for exclusive socket recv");
        require(mismatchReply->status == rsp::client::StreamStatus::NodeIdMismatch,
            "exclusive socket recv from a different node id should return NODEID_MISMATCH");

        const auto ownerGreeting = ownerClient->streamRecv(*exclusiveSocketId, static_cast<uint32_t>(exclusiveGreeting.size()));
        require(ownerGreeting.has_value(), "owner client should still be able to receive from its exclusive socket");
        require(*ownerGreeting == exclusiveGreeting,
            "owner client should receive the expected exclusive socket greeting");
        require(ownerClient->streamSend(*exclusiveSocketId, exclusivePayload),
            "owner client should still be able to send on its exclusive socket");
        const auto ownerResponse = ownerClient->streamRecv(*exclusiveSocketId, static_cast<uint32_t>(exclusiveResponse.size()));
        require(ownerResponse.has_value(), "owner client should still be able to receive the exclusive socket response");
        require(*ownerResponse == exclusiveResponse,
            "owner client should receive the expected exclusive socket response");
        require(ownerClient->streamClose(*exclusiveSocketId), "owner client should close its exclusive socket");
        exclusiveSocketServer.wait();

        TestSocketServer sharedSocketServer;
        const std::string sharedGreeting = "shared-greeting";
        const std::string sharedPayload = "shared-payload";
        const std::string sharedResponse = "shared-response";
        sharedSocketServer.start(sharedGreeting, sharedPayload, sharedResponse);

        const auto sharedSocketId = ownerClient->connectTCP(resourceServiceNodeId, sharedSocketServer.endpoint(), 0, 0, 0, false, true);
        require(sharedSocketId.has_value(), "owner client should receive a shared socket id");
        otherClient->registerStreamRoute(*sharedSocketId, resourceServiceNodeId);

        const auto sharedGreetingReply = otherClient->streamRecvEx(*sharedSocketId, 64);
        require(sharedGreetingReply.has_value(), "second client should receive a reply for shared socket recv");
        require(sharedGreetingReply->status == rsp::client::StreamStatus::Data,
            "shared socket recv from a different node id should succeed");
        require(!sharedGreetingReply->data.empty() && sharedGreetingReply->data == sharedGreeting,
            "second client should receive the shared socket greeting");
        require(otherClient->streamSend(*sharedSocketId, sharedPayload),
            "second client should be able to send on a shared socket");
        const auto sharedResponseReply = otherClient->streamRecvEx(*sharedSocketId, 64);
        require(sharedResponseReply.has_value(), "second client should receive the shared socket response");
        require(sharedResponseReply->status == rsp::client::StreamStatus::Data,
            "shared socket response should succeed for a different node id");
        require(!sharedResponseReply->data.empty() && sharedResponseReply->data == sharedResponse,
            "second client should receive the shared socket response");
        require(otherClient->streamClose(*sharedSocketId), "second client should be able to close a shared socket");
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

        auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
        auto client = rsp::client::RSPClient::create();

        const auto resourceServiceConnectionId =
            resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both endpoints for native listen bridge test");
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

            auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
            auto client = rsp::client::RSPClient::create();

            const auto resourceServiceConnectionId =
            resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
            const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

            require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
                "resource manager should authenticate both endpoints for shared socket option validation");
            const auto sharedAsyncReply = client->connectTCPEx(resourceServiceNodeId, "127.0.0.1:9", 0, 0, 0, true, true);
            require(sharedAsyncReply.has_value(),
                "share_socket combined with async_data should receive a reply");
            require(sharedAsyncReply->status == rsp::client::StreamStatus::InvalidFlags,
                "share_socket combined with async_data should return INVALID_FLAGS");

            const auto sharedUseStreamReply = client->connectTCPEx(resourceServiceNodeId, "127.0.0.1:9", 0, 0, 0, false, true, true);
            require(sharedUseStreamReply.has_value(),
                "share_socket combined with use_socket should receive a reply");
            require(sharedUseStreamReply->status == rsp::client::StreamStatus::InvalidFlags,
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

            auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
            auto client = rsp::client::RSPClient::create();

            const auto resourceServiceConnectionId =
                resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
            const auto clientConnectionId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

            require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
                "resource manager should authenticate both endpoints for listening option validation");
            const auto sharedChildrenReply =
                client->listenTCPEx(resourceServiceNodeId, listenerEndpoint, 0, false, false, true);
            require(sharedChildrenReply.has_value(),
                "share_child_sockets without async_accept should receive a reply");
            require(sharedChildrenReply->status == rsp::client::StreamStatus::InvalidFlags,
                "share_child_sockets without async_accept should return INVALID_FLAGS");

            const auto asyncChildrenReply =
                client->listenTCPEx(resourceServiceNodeId, listenerEndpoint, 0, false, false, false, false, true);
            require(asyncChildrenReply.has_value(),
                "children_async_data without async_accept should receive a reply");
            require(asyncChildrenReply->status == rsp::client::StreamStatus::InvalidFlags,
                "children_async_data without async_accept should return INVALID_FLAGS");

            require(resourceService->removeConnection(resourceServiceConnectionId),
                "resource service should remove its listening option validation connection");
            require(client->removeConnection(clientConnectionId),
                "client should remove its listening option validation connection");

            serverTransport->stop();
            }

        void testClientRequestsLoggingSchemaAndReceivesBsdSocketLogs() {
            auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
            TestResourceManager resourceManager({serverTransport});

            rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
            const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

            const std::string memoryChannel = "rm-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
            require(serverTransport->listen(memoryChannel), "memory transport listener should start");
            const std::string transportSpec = "memory:" + memoryChannel;
            const std::string listenerEndpoint = findAvailableEndpoint(35700, 35800);
            const std::string refusedEndpoint = findAvailableEndpoint(35800, 35900);

            auto resourceService = rsp::resource_service::BsdSocketsResourceService::create(std::move(resourceServiceKeyPair));
            auto socketClient = rsp::client::RSPClient::create();
            auto loggingClient = LoggingTestClient::create();

            const auto resourceServiceConnectionId =
                resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
            const auto socketClientConnectionId =
                socketClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();
            const auto loggingClientConnectionId =
                loggingClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

            require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 3; }),
                "resource manager should authenticate the resource service, socket client, and logging client");
            const auto resourceManagerNodeId = resourceManager.nodeId();
            require(waitForCondition([&resourceManager, &resourceServiceNodeId]() {
                return resourceManager.hasResourceAdvertisement(resourceServiceNodeId);
            }),
                "resource manager should ingest the bsd_sockets advertisement before logging discovery");

            require(loggingClient->queryResources(resourceManagerNodeId),
                "logging client should query for services advertising the logging schema");
            require(waitForCondition([&loggingClient]() { return loggingClient->pendingResourceQueryReplyCount() > 0; }),
                "logging client should receive a resource query reply for the logging schema");

            rsp::proto::ResourceQueryReply serviceReply;
            require(loggingClient->tryDequeueResourceQueryReply(serviceReply),
                "logging client should dequeue the discovered logging service");
            const auto discoveredLoggingNodeId = findServiceNodeIdByProto(
                serviceReply, "resource_service/bsd_sockets/bsd_sockets_logging.proto");
            require(discoveredLoggingNodeId.has_value(),
                std::string("resource query reply should include the advertised logging schema: ") +
                serviceReply.DebugString());
            require(*discoveredLoggingNodeId == resourceServiceNodeId,
                std::string("logging schema should be advertised by the bsd_sockets resource service node: ") +
                serviceReply.DebugString());

            require(loggingClient->querySchemas(resourceManagerNodeId,
                "resource_service/bsd_sockets/bsd_sockets_logging.proto"),
                "logging client should request the logging proto schema");
            require(waitForCondition([&loggingClient]() { return loggingClient->pendingSchemaReplyCount() > 0; }),
                "logging client should receive the logging schema reply");

            rsp::proto::SchemaReply schemaReply;
            require(loggingClient->tryDequeueSchemaReply(schemaReply),
                "logging client should dequeue the logging schema reply");
            require(schemaReply.schemas_size() == 1,
                "schema request for logging proto should return exactly one schema");
            require(schemaReply.schemas(0).proto_file_name() == "resource_service/bsd_sockets/bsd_sockets_logging.proto",
                "schema reply should return the logging proto descriptor set");
            require(!schemaReply.schemas(0).proto_file_descriptor_set().empty(),
                "logging proto descriptor set should be populated");

            rsp::resource_manager::SchemaSnapshot loggingSchemaSnapshot({schemaReply.schemas(0)});
            require(loggingSchemaSnapshot.findMessageDescriptor("rsp.proto.LogRecord") != nullptr,
                "logging schema should describe LogRecord for client-side filter construction");
            require(loggingSchemaSnapshot.findMessageDescriptor("rsp.proto.BsdSocketsListenStartedLog") != nullptr,
                "logging schema should describe listen-started payloads for client-side filter construction");
            require(loggingSchemaSnapshot.findMessageDescriptor("rsp.proto.BsdSocketsConnectFailedLog") != nullptr,
                "logging schema should describe connect-failed payloads for client-side filter construction");

            rsp::proto::LogSubscribeRequest lifecycleRequest;
            lifecycleRequest.set_payload_type_url("type.rsp/rsp.proto.BsdSocketsListenStartedLog");
            auto* lifecycleTerms = lifecycleRequest.mutable_filter()->mutable_all_of();
            *lifecycleTerms->add_terms() = makeFieldEqualsUint({"bind_port"}, parseEndpointPort(listenerEndpoint));
            lifecycleRequest.set_duration_ms(5000);
            require(loggingClient->subscribeToLogs(*discoveredLoggingNodeId, lifecycleRequest),
                "logging client should subscribe to bsd_sockets lifecycle logs");

            rsp::proto::LogSubscribeRequest errorRequest;
            errorRequest.set_payload_type_url("type.rsp/rsp.proto.BsdSocketsConnectFailedLog");
            auto* errorTerms = errorRequest.mutable_filter()->mutable_all_of();
            *errorTerms->add_terms() = makeFieldEqualsUint({"remote_port"}, parseEndpointPort(refusedEndpoint));
            errorRequest.set_duration_ms(5000);
            require(loggingClient->subscribeToLogs(*discoveredLoggingNodeId, errorRequest),
                "logging client should subscribe to bsd_sockets error logs");

            require(waitForCondition([&loggingClient]() { return loggingClient->pendingLogRecordCount() == 0; }),
                "log subscriptions should not emit records before socket activity");

            rsp::proto::LogSubscribeReply lifecycleReply;
            rsp::proto::LogSubscribeReply errorReply;
            require(waitForCondition([&loggingClient]() { return loggingClient->pendingSubscribeReplyCount() >= 2; }),
                "logging client should receive subscribe replies for both requested filters");
            require(loggingClient->tryDequeueSubscribeReply(lifecycleReply),
                "logging client should dequeue the lifecycle subscription reply");
            require(loggingClient->tryDequeueSubscribeReply(errorReply),
                "logging client should dequeue the error subscription reply");
            require(lifecycleReply.status() == rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED,
                "lifecycle log subscription should be accepted");
            require(errorReply.status() == rsp::proto::LOG_SUBSCRIPTION_STATUS_ACCEPTED,
                "error log subscription should be accepted");

            const auto listenSocketId = socketClient->listenTCP(resourceServiceNodeId, listenerEndpoint);
            require(listenSocketId.has_value(),
                "socket client should obtain a listening socket so the service emits lifecycle logs");

            const auto refusedSocketId = socketClient->connectTCP(resourceServiceNodeId, refusedEndpoint);
            require(!refusedSocketId.has_value(),
                "socket client should observe a refused TCP connect so the service emits an error log");

            require(waitForCondition([&loggingClient]() { return loggingClient->pendingLogRecordCount() >= 2; }),
                "logging client should receive the matching lifecycle and error log records");

            std::vector<rsp::proto::LogRecord> records;
            rsp::proto::LogRecord record;
            while (loggingClient->tryDequeueLogRecord(record)) {
                records.push_back(record);
            }

            require(records.size() == 2,
                "logging client should receive exactly the two matching log records");

            bool sawLifecycleRecord = false;
            bool sawErrorRecord = false;
            for (const auto& current : records) {
                if (current.payload().type_url() == "type.rsp/rsp.proto.BsdSocketsListenStartedLog") {
                    rsp::proto::BsdSocketsListenStartedLog payload;
                    require(current.payload().UnpackTo(&payload),
                        "listen-started log records should unpack to the typed bsd_sockets payload");
                    sawLifecycleRecord = payload.bind_port() == parseEndpointPort(listenerEndpoint) &&
                                         current.subscription_id().value() == lifecycleReply.subscription_id().value();
                }
                if (current.payload().type_url() == "type.rsp/rsp.proto.BsdSocketsConnectFailedLog") {
                    rsp::proto::BsdSocketsConnectFailedLog payload;
                    require(current.payload().UnpackTo(&payload),
                        "connect-failed log records should unpack to the typed bsd_sockets payload");
                    sawErrorRecord = payload.remote_port() == parseEndpointPort(refusedEndpoint) &&
                                     current.subscription_id().value() == errorReply.subscription_id().value();
                }
            }

            require(sawLifecycleRecord,
                "logging client should receive the subscribed typed listen-started record");
            require(sawErrorRecord,
                "logging client should receive the subscribed typed connect-failed record");
            require(socketClient->streamClose(*listenSocketId),
                "socket client should close the listening socket after receiving test logs");

            require(resourceService->removeConnection(resourceServiceConnectionId),
                "resource service should remove its logging integration test connection");
            require(socketClient->removeConnection(socketClientConnectionId),
                "socket client should remove its logging integration test connection");
            require(loggingClient->removeConnection(loggingClientConnectionId),
                "logging client should remove its logging integration test connection");

            serverTransport->stop();
        }

    void testNameRefreshUpdatesExpiryAndRecordIsReturned() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        const std::string memoryChannel = "rm-ns-refresh-" + std::to_string(rsp::GUID().high());
        require(serverTransport->listen(memoryChannel), "memory transport listener should start");
        const std::string transportSpec = "memory:" + memoryChannel;

        auto nameService = rsp::name_service::NameService::create();
        rsp::KeyPair clientKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID ownerNodeId = clientKeyPair.nodeID();
        auto client = rsp::client::RSPClient::create(std::move(clientKeyPair));

        const auto nsConnId = nameService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate name service and client");

        const rsp::NodeID nsNodeId = nameService->nodeId();
        const rsp::GUID type{};
        const rsp::GUID value{};
        const std::string name = "test.refresh.host";

        const auto createResult = client->nameCreate(nsNodeId, name, ownerNodeId, type, value);
        require(createResult.has_value() && createResult->status == rsp::client::NameResult::Status::Success,
            "nameCreate should succeed before refresh test");

        const auto refreshResult = client->nameRefresh(nsNodeId, name, ownerNodeId, type);
        require(refreshResult.has_value(),
            "nameRefresh should return a result");
        require(refreshResult->status == rsp::client::NameResult::Status::Success,
            "nameRefresh should return NAME_SUCCESS for an existing record");

        const auto readResult = client->nameRead(nsNodeId, name, ownerNodeId, type);
        require(readResult.has_value() && !readResult->records.empty(),
            "nameRead should return the refreshed record");

        require(nameService->removeConnection(nsConnId), "name service should remove connection");
        require(client->removeConnection(clientConnId), "client should remove connection");
        serverTransport->stop();
    }

    void testNameResolveReturnsReachableNode() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        const std::string memoryChannel = "rm-ns-resolve-" + std::to_string(rsp::GUID().high());
        require(serverTransport->listen(memoryChannel), "memory transport listener should start");
        const std::string transportSpec = "memory:" + memoryChannel;

        auto nameService = rsp::name_service::NameService::create();
        rsp::KeyPair serviceKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID serviceNodeId = serviceKeyPair.nodeID();
        auto serviceClient = rsp::client::RSPClient::create(std::move(serviceKeyPair));
        auto resolverClient = rsp::client::RSPClient::create();

        const auto nsConnId = nameService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto svcConnId = serviceClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();
        const auto resolverConnId = resolverClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 3; }),
            "resource manager should authenticate name service, service client, and resolver");

        const rsp::NodeID nsNodeId = nameService->nodeId();
        const rsp::GUID type{};
        const rsp::GUID value{};
        const std::string name = "test.resolve.host";

        const auto createResult = resolverClient->nameCreate(nsNodeId, name, serviceNodeId, type, value);
        require(createResult.has_value() && createResult->status == rsp::client::NameResult::Status::Success,
            "nameCreate should succeed for resolve test");

        const auto resolvedId = resolverClient->nameResolve(nsNodeId, name, type);
        require(resolvedId.has_value(),
            "nameResolve should return a node ID for the reachable service");
        require(*resolvedId == serviceNodeId,
            "nameResolve should return the service node's ID");

        require(nameService->removeConnection(nsConnId), "name service should remove connection");
        require(serviceClient->removeConnection(svcConnId), "service client should remove connection");
        require(resolverClient->removeConnection(resolverConnId), "resolver client should remove connection");
        serverTransport->stop();
    }

    void testNameResolveSkipsUnreachableNodes() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        const std::string memoryChannel = "rm-ns-skip-" + std::to_string(rsp::GUID().high());
        require(serverTransport->listen(memoryChannel), "memory transport listener should start");
        const std::string transportSpec = "memory:" + memoryChannel;

        auto nameService = rsp::name_service::NameService::create();
        rsp::KeyPair clientKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID realNodeId = clientKeyPair.nodeID();
        auto client = rsp::client::RSPClient::create(std::move(clientKeyPair));

        const auto nsConnId = nameService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        const auto clientConnId = client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();

        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate name service and client");

        const rsp::NodeID nsNodeId = nameService->nodeId();
        // A fake (non-existent) node ID that ping will never reach
        const rsp::NodeID fakeNodeId{0xDEADBEEFDEADBEEFULL, 0xCAFEBABECAFEBABEULL};
        const rsp::GUID type{};
        const rsp::GUID value{};
        const std::string name = "test.skip.host";

        // Register the fake node first so it sorts first if expiry is the same
        const auto createFake = client->nameCreate(nsNodeId, name, fakeNodeId, type, value);
        require(createFake.has_value() && createFake->status == rsp::client::NameResult::Status::Success,
            "nameCreate for fake node should succeed");

        // Small delay so the real record gets a later expiry (sorts first in descending order)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        const auto createReal = client->nameCreate(nsNodeId, name, realNodeId, type, value);
        require(createReal.has_value() && createReal->status == rsp::client::NameResult::Status::Success,
            "nameCreate for real node should succeed");

        const auto resolvedId = client->nameResolve(nsNodeId, name, type);
        require(resolvedId.has_value(),
            "nameResolve should return a node ID skipping the unreachable fake node");
        require(*resolvedId == realNodeId,
            "nameResolve should return the reachable real node ID");

        require(nameService->removeConnection(nsConnId), "name service should remove connection");
        require(client->removeConnection(clientConnId), "client should remove connection");
        serverTransport->stop();
    }

}  // namespace

int main() {
    try {
        testResourceServiceConnectsToResourceManager();
        testEmptyResourceAdvertisementClearsStoredResourceServiceRecords();
        testFailedRouteClearsStoredResourceServiceRecords();
        testClientDiscoversResourceServiceThroughResourceQuery();
        testClientDiscoversNameServiceThroughResourceQuery();
        testClientDiscoversTcpConnectResourceAndExchangesData();
        testClientExchangesTcpDataThroughResourceService();
        testClientReceivesAsyncSocketDataThroughResourceService();
        testClientExchangesTcpDataThroughNativeSocketBridge();
        testClientAcceptsTcpConnectionThroughResourceService();
        testClientReceivesAsyncAcceptedTcpConnectionThroughResourceService();
        testClientAcceptsTcpConnectionThroughNativeSocketBridge();
        testClientRequestsLoggingSchemaAndReceivesBsdSocketLogs();
        testClientListensWithNativeSocketBridgeAndAcceptsBidirectionalTraffic();
        testSocketOwnershipByNodeIdThroughResourceService();
        testSharedSocketRejectsUnsupportedOptions();
        testListeningSocketRejectsUnsupportedOptions();
        testNameRefreshUpdatesExpiryAndRecordIsReturned();
        testNameResolveReturnsReachableNode();
        testNameResolveSkipsUnreachableNodes();
        std::cout << "resource service test passed" << std::endl;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "resource service test failed: " << exception.what() << std::endl;
        return 1;
    }
}
