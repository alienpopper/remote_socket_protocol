#pragma once

#include "resource_service/bsd_sockets/resource_service_bsd_sockets.hpp"
#include "common/base_types.hpp"
#include "common/keypair.hpp"
#include "resource_service/httpd/httpd.pb.h"

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace rsp::resource_service {

// ---------------------------------------------------------------------------
// HttpdConfig
//
// Configuration passed to HttpdResourceService::create().  A real web-server
// integration (Express, nginx, etc.) will populate this via its own config
// loader and supply a custom createHttpConnection() override, so keep this
// struct minimal and extend it rather than modifying it.
// ---------------------------------------------------------------------------

struct HttpdConfig {
    // Transport spec used to connect to the RSP resource manager
    // (e.g. "tcp:127.0.0.1:7000").
    std::string rspTransport;

    // Address the built-in test HTTP server listens on.  When integrating with
    // a production web server, set this to the server's local listen address so
    // that createHttpConnection() can forward RSP streams to it.
    // Format: "host:port" (e.g. "127.0.0.1:8080").
    std::string listenAddress = "127.0.0.1:0";

    // Human-readable server name embedded in the service schema and HTTP
    // Server response header.  Defaults to "rsp-httpd".
    std::string serverName = "rsp-httpd";

    // Optional keypair files.  When present, rsp_httpd uses this identity so
    // its Node ID is deterministic across restarts.
    std::optional<std::string> keypairPublicKeyPath;
    std::optional<std::string> keypairPrivateKeyPath;
};

HttpdConfig loadHttpdConfig(const std::string& path);

// ---------------------------------------------------------------------------
// HttpdResourceService
//
// An RSP ResourceService that exposes an HTTP (or HTTPS) server as a stream
// endpoint.  Clients send ConnectHttp to get a raw TCP socket to the server;
// all HTTP framing flows as opaque bytes — the RSP layer never parses it.
//
// Extension guide
// ---------------
// To integrate an existing web server (e.g. Express, nginx):
//
//   1. Subclass HttpdResourceService.
//   2. Override createHttpConnection() to return a socket connected to your
//      server's local listen port instead of the built-in test server.
//   3. Optionally override buildResourceAdvertisement() to add extra metadata
//      (vhosts, accepted content types, ...) via your own proto extension.
//   4. Call connectToResourceManager() as normal; RSP handles the rest.
//
// The built-in test server (startBuiltinServer / handleBuiltinRequest) is
// intentionally small — it exists only to exercise the proto and wiring during
// unit/integration tests and should not be used in production.
// ---------------------------------------------------------------------------

class HttpdResourceService : public BsdSocketsResourceService {
public:
    using Ptr = std::shared_ptr<HttpdResourceService>;

    // Create an instance with a freshly generated P-256 key pair.
    static Ptr create(const HttpdConfig& cfg);

    // Create an instance with a caller-provided key pair.
    static Ptr create(rsp::KeyPair keyPair, const HttpdConfig& cfg);

    ~HttpdResourceService() override;

    // Returns the port the built-in test server is actually listening on
    // (useful after binding to port 0).  Returns 0 if the built-in server
    // is not running.
    uint16_t builtinServerPort() const;

protected:
    HttpdResourceService(rsp::KeyPair keyPair, const HttpdConfig& cfg);

    // ---------------------------------------------------------------------------
    // Extension points
    // ---------------------------------------------------------------------------

    // Called for every incoming ConnectHttp request.  The default implementation
    // connects to the built-in test server at cfg_.listenAddress.
    //
    // Override this to forward streams to a production web server.  The method
    // must return a TCPConnectionResult whose connection is already connected
    // (or return an empty result on failure — the base class will send
    // CONNECT_REFUSED to the caller).
    virtual TCPConnectionResult createHttpConnection(const rsp::proto::ConnectHttp& request);

    // Override to supply a custom ResourceAdvertisement (e.g. to add vhost
    // names, accepted content types, TLS capabilities).  The default
    // advertisement declares a single ConnectHttp endpoint with the server name
    // from HttpdConfig::serverName.
    rsp::proto::ResourceAdvertisement buildResourceAdvertisement() const override;

    // Override handleNodeSpecificMessage() if you need to handle additional
    // service-specific message types beyond ConnectHttp.  Always call the base
    // implementation for messages you do not recognise.
    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;

    const HttpdConfig& config() const { return cfg_; }

private:
    bool handleConnectHttp(const rsp::proto::RSPMessage& message);

    // ---------------------------------------------------------------------------
    // Built-in test HTTP/1.1 server
    // ---------------------------------------------------------------------------

    // Starts a minimal HTTP/1.1 server on cfg_.listenAddress.  Called once from
    // the constructor.  Sets builtinPort_ to the actual port after binding.
    void startBuiltinServer();

    // Handles a single accepted connection: reads the HTTP request line and
    // headers, then writes a synthetic 200 OK response.
    void handleBuiltinRequest(rsp::os::SocketHandle clientSock);

    HttpdConfig cfg_;
    bool builtinSocketsInitialized_{false};
    std::atomic<bool>    builtinStopping_{false};
    std::atomic<uint16_t> builtinPort_{0};
    rsp::os::SocketHandle builtinListenSock_{-1};
    std::thread builtinThread_;
};

}  // namespace rsp::resource_service
