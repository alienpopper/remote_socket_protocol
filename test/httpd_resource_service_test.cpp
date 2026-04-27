#define RSPCLIENT_STATIC

#include "client/cpp/rsp_client.hpp"
#include "resource_service/httpd/resource_service_httpd.hpp"

#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/transport/transport_memory.hpp"
#include "resource_manager/resource_manager.hpp"
#include "os/os_socket.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

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

bool sendAll(rsp::os::SocketHandle socket, const std::string& data) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const int sent = rsp::os::sendSocket(socket, bytes, static_cast<uint32_t>(remaining));
        if (sent <= 0) {
            return false;
        }
        bytes += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    require(file.is_open(), "test should create config file: " + path.string());
    file << text;
}

std::optional<rsp::NodeID> fromProtoNodeId(const rsp::proto::NodeId& nodeId) {
    if (nodeId.value().size() != 16) {
        return std::nullopt;
    }
    uint64_t high = 0, low = 0;
    std::memcpy(&high, nodeId.value().data(), sizeof(high));
    std::memcpy(&low,  nodeId.value().data() + sizeof(high), sizeof(low));
    return rsp::NodeID(high, low);
}

// ---------------------------------------------------------------------------
// testHttpdRegistersWithResourceManager
//
// HttpdResourceService must connect, authenticate, and send a resource
// advertisement that contains a "httpd.proto" schema entry with the
// expected accepted_type_urls.
// ---------------------------------------------------------------------------

void testHttpdRegistersWithResourceManager() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();

    const std::string channel =
        "httpd-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(channel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + channel;

    // Stand-alone resource manager backed by the memory transport.
    class TestRM : public rsp::resource_manager::ResourceManager {
    public:
        using rsp::resource_manager::ResourceManager::ResourceManager;
        rsp::NodeID nodeId() const { return keyPair().nodeID(); }
    };
    TestRM resourceManager({serverTransport});

    rsp::resource_service::HttpdConfig cfg;
    cfg.rspTransport   = transportSpec;
    cfg.listenAddress  = "127.0.0.1:0";  // let the OS pick a port
    cfg.serverName     = "test-httpd";

    auto httpdService = rsp::resource_service::HttpdResourceService::create(cfg);
    const rsp::NodeID httpdNodeId = httpdService->nodeId();

    const auto connId = httpdService->connectToResourceManager(
        transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    require(connId != rsp::GUID{}, "httpd service should connect to the resource manager");

    require(waitForCondition([&resourceManager, &httpdNodeId]() {
                return resourceManager.hasResourceAdvertisement(httpdNodeId);
            }),
            "resource manager should store the httpd service advertisement");

    require(httpdService->builtinServerPort() != 0,
            "httpd built-in server should be bound to a non-zero port");

    httpdService->removeConnection(connId);
    serverTransport->stop();
}

void testHttpdLoadsKeypairFromConfig() {
    const std::filesystem::path testDir = std::filesystem::path("build") / "test" / "httpd_keypair";
    const std::filesystem::path configPath = testDir / "rsp_httpd.conf.json";
    const std::string publicKeyPath = "test/keys/test_public.pem";
    const std::string privateKeyPath = "test/keys/test_private.pem";

    writeTextFile(configPath,
                  "{\n"
                  "  \"rsp_transport\": \"memory:httpd-keypair-test\",\n"
                  "  \"listen_address\": \"127.0.0.1:0\",\n"
                  "  \"server_name\": \"test-httpd-keypair\",\n"
                  "  \"keypair\": [\"" + publicKeyPath + "\", \"" + privateKeyPath + "\"]\n"
                  "}\n");

    const auto cfg = rsp::resource_service::loadHttpdConfig(configPath.string());
    auto httpdService = rsp::resource_service::HttpdResourceService::create(cfg);
    const auto expectedNodeId = rsp::KeyPair::nodeIDFromPublicKeyFile(publicKeyPath);

    require(httpdService->nodeId() == expectedNodeId,
            "httpd service node ID should come from configured keypair");
}

// ---------------------------------------------------------------------------
// testClientDiscoversHttpdThroughResourceQuery
//
// A plain RSPClient queries the RM for services whose schema proto file is
// "httpd.proto" and verifies the reply identifies the httpd service node.
// ---------------------------------------------------------------------------

void testClientDiscoversHttpdThroughResourceQuery() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();

    const std::string channel =
        "httpd-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(channel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + channel;

    class TestRM : public rsp::resource_manager::ResourceManager {
    public:
        using rsp::resource_manager::ResourceManager::ResourceManager;
        rsp::NodeID nodeId() const { return keyPair().nodeID(); }
    };
    TestRM resourceManager({serverTransport});

    rsp::resource_service::HttpdConfig cfg;
    cfg.rspTransport  = transportSpec;
    cfg.listenAddress = "127.0.0.1:0";
    cfg.serverName    = "test-httpd";

    auto httpdService = rsp::resource_service::HttpdResourceService::create(cfg);
    const rsp::NodeID httpdNodeId = httpdService->nodeId();

    const auto httpdConnId = httpdService->connectToResourceManager(
        transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    require(httpdConnId != rsp::GUID{}, "httpd service should connect");

    auto client = rsp::client::RSPClient::create();
    const auto clientConnId = client->connectToResourceManager(
        transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();
    require(client->hasConnection(clientConnId), "client should connect");

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both connections");
    require(waitForCondition([&resourceManager, &httpdNodeId]() {
                return resourceManager.hasResourceAdvertisement(httpdNodeId);
            }),
            "resource manager should store the httpd advertisement before query");

    const auto rmNodeId = client->peerNodeID(clientConnId);
    require(rmNodeId.has_value(), "client should know the resource manager node id");

    require(client->queryResources(*rmNodeId, "service.proto_file_name = \"httpd.proto\""),
            "client should send a resource query for httpd schemas");
    require(waitForCondition([&client]() { return client->pendingResourceQueryReplyCount() == 1; }),
            "client should receive a resource query reply");

    rsp::client::ResourceQueryResult queryReply;
    require(client->tryDequeueResourceQueryReply(queryReply),
            "client should dequeue the httpd resource query reply");
    require(queryReply.services.size() == 1,
            "httpd query should return exactly one service");

    const auto& svc = queryReply.services[0];
    require(svc.protoFileName == "httpd.proto",
            "discovered service should identify itself via httpd.proto");

    bool hasConnectHttp = false;
    for (const auto& url : svc.acceptedTypeUrls) {
        if (url == "type.rsp/rsp.proto.ConnectHttp") {
            hasConnectHttp = true;
            break;
        }
    }
    require(hasConnectHttp, "httpd schema should list ConnectHttp as an accepted type");

    require(svc.nodeId == httpdNodeId,
            "discovered httpd node id should match the service");

    httpdService->removeConnection(httpdConnId);
    client->removeConnection(clientConnId);
    serverTransport->stop();
}

// ---------------------------------------------------------------------------
// testClientConnectsToHttpdAndExchangesData
//
// A client sends ConnectHttp to the httpd service, then sends a raw HTTP/1.1
// request over StreamSend and reads the 200 OK response via StreamRecv.
// This exercises the full stream path through the RSP layer into the
// built-in HTTP server.
// ---------------------------------------------------------------------------

void testClientConnectsToHttpdAndExchangesData() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();

    const std::string channel =
        "httpd-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(channel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + channel;

    class TestRM : public rsp::resource_manager::ResourceManager {
    public:
        using rsp::resource_manager::ResourceManager::ResourceManager;
        rsp::NodeID nodeId() const { return keyPair().nodeID(); }
    };
    TestRM resourceManager({serverTransport});

    rsp::resource_service::HttpdConfig cfg;
    cfg.rspTransport  = transportSpec;
    cfg.listenAddress = "127.0.0.1:0";
    cfg.serverName    = "test-httpd";

    auto httpdService = rsp::resource_service::HttpdResourceService::create(cfg);
    const rsp::NodeID httpdNodeId = httpdService->nodeId();

    const auto httpdConnId = httpdService->connectToResourceManager(
        transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    require(httpdConnId != rsp::GUID{}, "httpd service should connect");

    auto client = rsp::client::RSPClient::create();
    const auto clientConnId = client->connectToResourceManager(
        transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();
    require(client->hasConnection(clientConnId), "client should connect");

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both connections");
    require(waitForCondition([&resourceManager, &httpdNodeId]() {
                return resourceManager.hasResourceAdvertisement(httpdNodeId);
            }),
            "resource manager should have the httpd advertisement");

    // --- Send ConnectHttp ---
    const auto connectReply = client->connectHttpEx(httpdNodeId);
    require(connectReply.has_value(), "client should receive a stream reply for ConnectHttp");
    require(connectReply->status == rsp::client::StreamStatus::Success,
            "ConnectHttp should succeed: " +
                (!connectReply->message.empty() ? connectReply->message : "(no message)"));

    // Extract the stream ID assigned by the client.
    const rsp::GUID streamId = connectReply->streamId;

    // --- Send a raw HTTP/1.1 GET request over the stream ---
    const std::string httpRequest =
        "GET /test-page HTTP/1.1\r\n"
        "Host: test-httpd\r\n"
        "Connection: close\r\n"
        "\r\n";

    require(client->streamSend(streamId, httpRequest),
            "client should send HTTP request over the RSP stream");

    // --- Read the response ---
    const auto recvReply = client->streamRecvEx(streamId, 4096, 2000);
    require(recvReply.has_value(), "client should receive a stream reply for streamRecv");
    require(recvReply->status == rsp::client::StreamStatus::Success ||
                recvReply->status == rsp::client::StreamStatus::Data,
            "streamRecv reply should indicate success or stream data");
    require(!recvReply->data.empty(), "streamRecv reply should contain response data");

    const std::string responseData = recvReply->data;
    require(responseData.find("HTTP/1.1 200 OK") != std::string::npos,
            "HTTP response should contain '200 OK'");
    require(responseData.find("test-httpd") != std::string::npos,
            "HTTP response body should contain the server name");
    require(responseData.find("/test-page") != std::string::npos,
            "HTTP response body should echo the request path");

    // --- Clean up ---
    client->streamClose(streamId);
    httpdService->removeConnection(httpdConnId);
    client->removeConnection(clientConnId);
    serverTransport->stop();
}

// ---------------------------------------------------------------------------
// testClientConnectsToHttpdSocketAndExchangesData
//
// Exercises the native socket bridge used by Chromium's rsp:// loader: the
// client sends ConnectHttp with async stream data, bridges the RSP stream to a
// local socket pair, and performs a plain HTTP/1.1 exchange over that socket.
// ---------------------------------------------------------------------------

void testClientConnectsToHttpdSocketAndExchangesData() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();

    const std::string channel =
        "httpd-socket-test-" + std::to_string(rsp::GUID().high()) + "-" + std::to_string(rsp::GUID().low());
    require(serverTransport->listen(channel), "memory transport listener should start");
    const std::string transportSpec = "memory:" + channel;

    class TestRM : public rsp::resource_manager::ResourceManager {
    public:
        using rsp::resource_manager::ResourceManager::ResourceManager;
    };
    TestRM resourceManager({serverTransport});

    rsp::resource_service::HttpdConfig cfg;
    cfg.rspTransport  = transportSpec;
    cfg.listenAddress = "127.0.0.1:0";
    cfg.serverName    = "test-httpd-socket";

    auto httpdService = rsp::resource_service::HttpdResourceService::create(cfg);
    const rsp::NodeID httpdNodeId = httpdService->nodeId();

    const auto httpdConnId = httpdService->connectToResourceManager(
        transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    require(httpdConnId != rsp::GUID{}, "httpd service should connect");

    auto client = rsp::client::RSPClient::create();
    const auto clientConnId = client->connectToResourceManager(
        transportSpec, rsp::message_queue::kAsciiHandshakeEncoding).value();
    require(client->hasConnection(clientConnId), "client should connect");

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "resource manager should authenticate both connections");
    require(waitForCondition([&resourceManager, &httpdNodeId]() {
                return resourceManager.hasResourceAdvertisement(httpdNodeId);
            }),
            "resource manager should have the httpd advertisement");

    const auto socket = client->connectHttpSocket(httpdNodeId, 2000, "test-httpd-socket");
    require(socket.has_value(), "client should open a bridged HTTP socket");

    const std::string httpRequest =
        "GET /test-page HTTP/1.1\r\n"
        "Host: test-httpd-socket\r\n"
        "Connection: close\r\n"
        "\r\n";
    require(sendAll(*socket, httpRequest), "client should send HTTP request through bridged socket");

    std::array<uint8_t, 4096> buffer{};
    const int received = rsp::os::recvSocket(*socket, buffer.data(), static_cast<uint32_t>(buffer.size()));
    require(received > 0, "client should receive HTTP response through bridged socket");
    const std::string response(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(received));

    require(response.find("HTTP/1.1 200 OK") != std::string::npos,
            "HTTP socket response should contain '200 OK'");
    require(response.find("test-httpd-socket") != std::string::npos,
            "HTTP socket response body should contain the server name");
    require(response.find("/test-page") != std::string::npos,
            "HTTP socket response body should echo the request path");

    rsp::os::closeSocket(*socket);

    const auto notFoundSocket = client->connectHttpSocket(httpdNodeId, 2000, "test-httpd-socket");
    require(notFoundSocket.has_value(), "client should open a second bridged HTTP socket");

    const std::string notFoundRequest =
        "GET /missing HTTP/1.1\r\n"
        "Host: test-httpd-socket\r\n"
        "Connection: close\r\n"
        "\r\n";
    require(sendAll(*notFoundSocket, notFoundRequest),
            "client should send 404 HTTP request through bridged socket");

    std::array<uint8_t, 4096> notFoundBuffer{};
    const int notFoundReceived = rsp::os::recvSocket(
        *notFoundSocket, notFoundBuffer.data(), static_cast<uint32_t>(notFoundBuffer.size()));
    require(notFoundReceived > 0, "client should receive 404 HTTP response through bridged socket");
    const std::string notFoundResponse(reinterpret_cast<const char*>(notFoundBuffer.data()),
                                       static_cast<std::size_t>(notFoundReceived));

    require(notFoundResponse.find("HTTP/1.1 404 Not Found") != std::string::npos,
            "HTTP socket response should contain '404 Not Found'");
    require(notFoundResponse.find("/missing") != std::string::npos,
            "HTTP 404 response body should echo the request path");

    rsp::os::closeSocket(*notFoundSocket);
    httpdService->removeConnection(httpdConnId);
    client->removeConnection(clientConnId);
    serverTransport->stop();
}

}  // namespace

int main() {
    try {
        testHttpdRegistersWithResourceManager();
        testHttpdLoadsKeypairFromConfig();
        testClientDiscoversHttpdThroughResourceQuery();
        testClientConnectsToHttpdAndExchangesData();
        testClientConnectsToHttpdSocketAndExchangesData();
        std::cout << "httpd resource service test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "httpd resource service test failed: " << ex.what() << std::endl;
        return 1;
    }
}
