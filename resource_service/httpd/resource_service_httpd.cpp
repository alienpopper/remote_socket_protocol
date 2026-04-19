// resource_service_httpd.cpp — HTTP server forwarder over RSP
//
// Registers an HTTP (or HTTPS) server with the RSP resource manager.
// When a client sends ConnectHttp, the service connects the resulting RSP
// stream to a local HTTP server socket.  All HTTP framing flows as opaque
// bytes via StreamSend / StreamRecv — the RSP layer never parses it.
//
// The built-in test server (HttpdResourceService::startBuiltinServer) is
// intentionally minimal: it speaks HTTP/1.1 and returns synthetic 200 OK
// responses so that the proto and stream-wiring mechanics can be tested
// without any external dependency.  It is NOT suitable for production use.
//
// Production integration
// ----------------------
// Subclass HttpdResourceService and override createHttpConnection() to
// return a socket connected to your real web server process.  The rest of
// the RSP stream machinery is inherited from BsdSocketsResourceService and
// requires no changes.
//
// Usage (standalone binary):
//   rsp_httpd [/path/to/rsp_httpd.conf.json]
//
// Default config path: /etc/rsp-httpd/rsp_httpd.conf.json

#include "resource_service/httpd/resource_service_httpd.hpp"

#include "common/keypair.hpp"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/service_message.hpp"
#include "common/transport/transport_tcp.hpp"
#include "os/os_socket.hpp"
#include "resource_service/schema_helpers.hpp"
#include "third_party/json/single_include/nlohmann/json.hpp"

#include "resource_service/httpd/httpd.pb.h"
#include "resource_service/httpd/httpd_desc.hpp"
#include "resource_service/bsd_sockets/bsd_sockets.pb.h"

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

using nlohmann::json;

namespace {

void log(const std::string& msg) {
    std::cerr << "[rsp-httpd] " << msg << '\n';
}

// Parse "host:port" into address and port components.
bool parseHostPort(const std::string& hostPort, std::string& address, uint16_t& port) {
    const auto colon = hostPort.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    address = hostPort.substr(0, colon);
    try {
        const int p = std::stoi(hostPort.substr(colon + 1));
        if (p < 0 || p > 65535) {
            return false;
        }
        port = static_cast<uint16_t>(p);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

namespace rsp::resource_service {

HttpdConfig loadHttpdConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    json j;
    file >> j;

    HttpdConfig cfg;
    cfg.rspTransport   = j.at("rsp_transport").get<std::string>();
    if (j.contains("listen_address")) cfg.listenAddress = j["listen_address"].get<std::string>();
    if (j.contains("server_name"))    cfg.serverName    = j["server_name"].get<std::string>();
    return cfg;
}

// ---------------------------------------------------------------------------
// HttpdResourceService
// ---------------------------------------------------------------------------

HttpdResourceService::Ptr HttpdResourceService::create(const HttpdConfig& cfg) {
    return Ptr(new HttpdResourceService(rsp::KeyPair::generateP256(), cfg));
}

HttpdResourceService::HttpdResourceService(rsp::KeyPair keyPair, const HttpdConfig& cfg)
    : BsdSocketsResourceService(std::move(keyPair)), cfg_(cfg) {
    startBuiltinServer();
}

HttpdResourceService::~HttpdResourceService() {
    // Signal the accept loop to stop, then close the listener to unblock accept().
    builtinStopping_.store(true);
    if (rsp::os::isValidSocket(builtinListenSock_)) {
        rsp::os::closeSocket(builtinListenSock_);
        builtinListenSock_ = rsp::os::invalidSocket();
    }
    if (builtinThread_.joinable()) {
        builtinThread_.join();
    }
}

uint16_t HttpdResourceService::builtinServerPort() const {
    return builtinPort_.load();
}

// ---------------------------------------------------------------------------
// Extension point: createHttpConnection
//
// The default implementation connects to the built-in test server at
// cfg_.listenAddress (updated to the actual port after bind).
//
// Override this to forward RSP streams to a production HTTP server instead.
// ---------------------------------------------------------------------------

HttpdResourceService::TCPConnectionResult
HttpdResourceService::createHttpConnection(const rsp::proto::ConnectHttp& /*request*/) {
    // Use the actual port the built-in server bound to (may differ from the
    // configured port when cfg_.listenAddress ended with ":0").
    const uint16_t port = builtinPort_.load();
    if (port == 0) {
        log("Built-in server is not running; cannot create connection");
        return {};
    }

    const std::string hostPort = "127.0.0.1:" + std::to_string(port);
    return createTCPConnection(hostPort, /*attempts=*/3, /*retryMs=*/50);
}

// ---------------------------------------------------------------------------
// Advertisement
// ---------------------------------------------------------------------------

rsp::proto::ResourceAdvertisement HttpdResourceService::buildResourceAdvertisement() const {
    rsp::proto::ResourceAdvertisement advertisement;

    // No TCP connect/listen records — clients use ConnectHttp, not raw TCP.
    // The schema tells resource-query clients what message types this node accepts.
    *advertisement.add_schemas() = buildServiceSchema(
        "httpd.proto",
        rsp::schema::kHttpdDescriptor,
        rsp::schema::kHttpdDescriptorSize,
        1,
        {
            "type.rsp/rsp.proto.ConnectHttp",
            "type.rsp/rsp.proto.StreamSend",
            "type.rsp/rsp.proto.StreamRecv",
            "type.rsp/rsp.proto.StreamClose",
        });

    return advertisement;
}

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

bool HttpdResourceService::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (rsp::hasServiceMessage<rsp::proto::ConnectHttp>(message)) {
        return handleConnectHttp(message);
    }

    // Reject raw TCP messages — the HTTP service does not expose raw sockets.
    if (rsp::hasServiceMessage<rsp::proto::ConnectTCPRequest>(message) ||
        rsp::hasServiceMessage<rsp::proto::ListenTCPRequest>(message) ||
        rsp::hasServiceMessage<rsp::proto::AcceptTCP>(message)) {
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR,
                                           "UNIMPLEMENTED: rsp_httpd does not expose raw TCP sockets"));
    }

    // StreamSend / StreamRecv / StreamClose — delegate to base class which
    // manages the registered socket state.
    return BsdSocketsResourceService::handleNodeSpecificMessage(message);
}

bool HttpdResourceService::handleConnectHttp(const rsp::proto::RSPMessage& message) {
    rsp::proto::ConnectHttp request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    if (!request.has_stream_id()) {
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS,
                                           "stream_id is required"));
    }

    const auto socketId = fromProtoStreamId(request.stream_id());
    if (!socketId.has_value()) {
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS,
                                           "invalid stream_id"));
    }

    const bool asyncData   = request.has_async_data()   && request.async_data();
    const bool shareSocket = request.has_share_socket()  && request.share_socket();

    if (shareSocket && asyncData) {
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS,
                                           "share_socket cannot be combined with async_data",
                                           &*socketId));
    }

    auto tcpResult = createHttpConnection(request);
    if (tcpResult.connection == nullptr) {
        log("Failed to connect to HTTP server");
        return send(makeStreamReplyMessage(message, rsp::proto::CONNECT_REFUSED,
                                           "HTTP server connection failed", &*socketId));
    }

    return registerConnectedSocket(message, std::move(tcpResult), *socketId, asyncData, shareSocket);
}

// ---------------------------------------------------------------------------
// Built-in test HTTP/1.1 server
// ---------------------------------------------------------------------------

void HttpdResourceService::startBuiltinServer() {
    std::string address;
    uint16_t    port = 0;
    if (!parseHostPort(cfg_.listenAddress, address, port)) {
        address = "127.0.0.1";
        port    = 0;
    }

    builtinListenSock_ = rsp::os::createTcpListener(address, port, 32);
    if (!rsp::os::isValidSocket(builtinListenSock_)) {
        throw std::runtime_error("[rsp-httpd] Failed to bind built-in HTTP server on " + cfg_.listenAddress);
    }

    builtinPort_.store(rsp::os::getSocketPort(builtinListenSock_));
    log("Built-in HTTP server listening on " + address + ":" + std::to_string(builtinPort_.load()));

    builtinThread_ = std::thread([this]() {
        while (!builtinStopping_.load()) {
            const rsp::os::SocketHandle client = rsp::os::acceptSocket(builtinListenSock_);
            if (!rsp::os::isValidSocket(client)) {
                break;  // listener was closed
            }

            // Handle each connection in a detached thread so the accept loop
            // is never blocked by a slow client.
            std::thread([this, client]() {
                handleBuiltinRequest(client);
                rsp::os::closeSocket(client);
            }).detach();
        }
    });
}

void HttpdResourceService::handleBuiltinRequest(rsp::os::SocketHandle clientSock) {
    // Read until we see the blank line that terminates the HTTP request header.
    std::string requestBuf;
    requestBuf.reserve(512);
    std::array<uint8_t, 256> chunk{};

    while (true) {
        const int n = rsp::os::recvSocket(clientSock, chunk.data(), static_cast<uint32_t>(chunk.size()));
        if (n <= 0) {
            return;  // connection closed before we got a complete request
        }
        requestBuf.append(reinterpret_cast<const char*>(chunk.data()), static_cast<std::size_t>(n));
        if (requestBuf.find("\r\n\r\n") != std::string::npos ||
            requestBuf.find("\n\n")     != std::string::npos) {
            break;
        }
        if (requestBuf.size() > 16384) {
            break;  // guard against excessively large headers
        }
    }

    // Parse the request line (first line) to echo the path back in the body.
    std::string method, path, version;
    std::istringstream firstLine(requestBuf);
    firstLine >> method >> path >> version;
    if (path.empty()) {
        path = "/";
    }

    const std::string body =
        "Hello from " + cfg_.serverName + "\r\n"
        "Path: " + path + "\r\n";

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: text/plain\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Server: " << cfg_.serverName << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << body;

    const std::string responseStr = response.str();
    const auto* data = reinterpret_cast<const uint8_t*>(responseStr.data());
    std::size_t remaining = responseStr.size();
    while (remaining > 0) {
        const int sent = rsp::os::sendSocket(clientSock, data, static_cast<uint32_t>(remaining));
        if (sent <= 0) {
            break;
        }
        data      += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
}

}  // namespace rsp::resource_service
