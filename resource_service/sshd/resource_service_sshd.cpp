// resource_service_sshd.cpp - OpenSSH server forwarder over RSP
//
// Registers with an RSP resource manager as a ResourceService.
// When a client sends a TCP_CONNECT request, spawns `sshd -i` in inetd mode
// and bridges RSP socket data to sshd's stdin/stdout via a socketpair.
// Responds to TCP_LISTEN requests with UNIMPLEMENTED.
//
// Compatible with systemd: logs to stderr (journald), handles SIGTERM/SIGCHLD.
//
// Usage:
//   rsp_sshd [/path/to/rsp_sshd.conf.json]
//
// Default config path: /etc/rsp-sshd/rsp_sshd.conf.json

#include "resource_service/sshd/resource_service_sshd.hpp"

#define RSPCLIENT_STATIC
#include "client/cpp/rsp_client.hpp"

#include "common/keypair.hpp"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "common/service_message.hpp"
#include "common/transport/transport.hpp"
#include "resource_service/schema_helpers.hpp"
#include "third_party/json/single_include/nlohmann/json.hpp"

#include "resource_service/bsd_sockets/bsd_sockets.pb.h"
#include "resource_service/sshd/sshd.pb.h"
#include "resource_service/sshd/sshd_desc.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/resource.h>

using nlohmann::json;

namespace {

void log(const std::string& msg) {
    std::cerr << "[rsp-sshd] " << msg << '\n';
}

}  // namespace

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

namespace rsp::resource_service {

SshdConfig loadSshdConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    json j;
    file >> j;

    SshdConfig cfg;
    cfg.rspTransport = j.at("rsp_transport").get<std::string>();
    if (j.contains("sshd_path"))   cfg.sshdPath   = j["sshd_path"].get<std::string>();
    if (j.contains("sshd_config")) cfg.sshdConfig = j["sshd_config"].get<std::string>();
    if (j.contains("sshd_debug"))  cfg.sshdDebug  = j["sshd_debug"].get<bool>();
    if (j.contains("ns_hostname")) cfg.nsHostname  = j["ns_hostname"].get<std::string>();
    if (j.contains("keypair")) {
        const auto& kp = j["keypair"];
        if (!kp.is_array() || kp.size() != 2) {
            throw std::runtime_error("\"keypair\" must be [\"<public_key_path>\", \"<private_key_path>\"]");
        }
        cfg.keypairPublicKeyPath  = kp[0].get<std::string>();
        cfg.keypairPrivateKeyPath = kp[1].get<std::string>();
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// SshdConnection
//
// Implements rsp::transport::Connection over one end of a socketpair.
// The other end is inherited as stdin/stdout by a child sshd -i process.
// ---------------------------------------------------------------------------

class SshdConnection : public rsp::transport::Connection {
public:
    SshdConnection(int appFd, pid_t childPid)
        : appFd_(appFd), childPid_(childPid) {}

    ~SshdConnection() override { close(); }

    int send(const rsp::Buffer& data) override {
        if (appFd_ < 0) return -1;
        return static_cast<int>(::write(appFd_, data.data(), data.size()));
    }

    int recv(rsp::Buffer& buffer) override {
        if (appFd_ < 0) return -1;
        return static_cast<int>(::read(appFd_, buffer.data(), buffer.size()));
    }

    void close() override {
        if (appFd_ >= 0) {
            ::close(appFd_);
            appFd_ = -1;
        }
        if (childPid_ > 0) {
            ::kill(childPid_, SIGTERM);
            childPid_ = -1;
        }
    }

private:
    int   appFd_;
    pid_t childPid_;
};

// ---------------------------------------------------------------------------
// SshdResourceService
// ---------------------------------------------------------------------------

std::shared_ptr<SshdResourceService> SshdResourceService::create(const SshdConfig& cfg) {
    rsp::KeyPair keyPair = (!cfg.keypairPublicKeyPath.empty() && !cfg.keypairPrivateKeyPath.empty())
        ? rsp::KeyPair::readFromDisk(cfg.keypairPrivateKeyPath, cfg.keypairPublicKeyPath)
        : rsp::KeyPair::generateP256();
    return std::shared_ptr<SshdResourceService>(
        new SshdResourceService(std::move(keyPair), cfg));
}

SshdResourceService::SshdResourceService(rsp::KeyPair keyPair, const SshdConfig& cfg)
    : BsdSocketsResourceService(std::move(keyPair)), cfg_(cfg) {}

rsp::proto::ResourceAdvertisement SshdResourceService::buildResourceAdvertisement() const {
    rsp::proto::ResourceAdvertisement advertisement;
    auto* record = advertisement.add_records();
    auto* sshd = record->mutable_sshd();
    if (!cfg_.sshdConfig.empty()) {
        sshd->set_server_name(cfg_.sshdConfig);
    }

    *advertisement.add_schemas() = buildServiceSchema(
        "sshd.proto",
        rsp::schema::kSshdDescriptor,
        rsp::schema::kSshdDescriptorSize,
        1,
        {
            "type.rsp/rsp.proto.ConnectSshd",
            "type.rsp/rsp.proto.StreamSend",
            "type.rsp/rsp.proto.StreamRecv",
            "type.rsp/rsp.proto.StreamClose",
        });

    return advertisement;
}

bool SshdResourceService::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (rsp::hasServiceMessage<rsp::proto::ConnectSshd>(message)) {
        return handleConnectSshd(message);
    }

    // Reject bsd_sockets-specific messages that sshd does not support.
    if (rsp::hasServiceMessage<rsp::proto::ConnectTCPRequest>(message) ||
        rsp::hasServiceMessage<rsp::proto::ListenTCPRequest>(message) ||
        rsp::hasServiceMessage<rsp::proto::AcceptTCP>(message)) {
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR,
                                           "UNIMPLEMENTED: rsp_sshd does not support this request"));
    }

    // Delegate socket_send, socket_recv, socket_close to the parent.
    return BsdSocketsResourceService::handleNodeSpecificMessage(message);
}

bool SshdResourceService::handleConnectSshd(const rsp::proto::RSPMessage& message) {
    rsp::proto::ConnectSshd request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    if (!request.has_stream_id()) {
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS, "socket_number is required"));
    }

    const auto socketId = fromProtoStreamId(request.stream_id());
    if (!socketId.has_value()) {
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS, "invalid socket_number"));
    }

    const bool asyncData = request.has_async_data() && request.async_data();
    const bool shareSocket = request.has_share_socket() && request.share_socket();

    if (shareSocket) {
        if (asyncData) {
            return send(makeStreamReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_socket cannot be combined with async_data",
                                               &*socketId));
        }
        if (request.has_use_socket() && request.use_socket()) {
            return send(makeStreamReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_socket cannot be combined with use_socket",
                                               &*socketId));
        }
    }

    // Create a socketpair and fork sshd -i on the child end.
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        log("socketpair failed: " + std::string(strerror(errno)));
        return send(makeStreamReplyMessage(message, rsp::proto::CONNECT_REFUSED,
                                           "socketpair failed", &*socketId));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        log("fork failed: " + std::string(strerror(errno)));
        ::close(fds[0]);
        ::close(fds[1]);
        return send(makeStreamReplyMessage(message, rsp::proto::CONNECT_REFUSED,
                                           "fork failed", &*socketId));
    }

    if (pid == 0) {
        // Child: dup fds[1] onto stdin/stdout and exec sshd -i.
        ::close(fds[0]);
        if (::dup2(fds[1], STDIN_FILENO) < 0 || ::dup2(fds[1], STDOUT_FILENO) < 0) {
            std::cerr << "[rsp-sshd] dup2 failed: " << strerror(errno) << '\n';
            ::_exit(1);
        }
        if (fds[1] != STDIN_FILENO && fds[1] != STDOUT_FILENO) {
            ::close(fds[1]);
        }

        // Close all file descriptors inherited from the parent (RM sockets, etc.)
        struct rlimit rl{};
        ::getrlimit(RLIMIT_NOFILE, &rl);
        const int maxfd = static_cast<int>(rl.rlim_cur);
        for (int fd = STDERR_FILENO + 1; fd < maxfd; fd++) {
            ::close(fd);
        }

        if (cfg_.sshdDebug) {
            if (cfg_.sshdConfig.empty()) {
                ::execl(cfg_.sshdPath.c_str(), cfg_.sshdPath.c_str(), "-i", "-d", nullptr);
            } else {
                ::execl(cfg_.sshdPath.c_str(), cfg_.sshdPath.c_str(), "-i", "-d",
                        "-f", cfg_.sshdConfig.c_str(), nullptr);
            }
        } else {
            if (cfg_.sshdConfig.empty()) {
                ::execl(cfg_.sshdPath.c_str(), cfg_.sshdPath.c_str(), "-i", nullptr);
            } else {
                ::execl(cfg_.sshdPath.c_str(), cfg_.sshdPath.c_str(), "-i",
                        "-f", cfg_.sshdConfig.c_str(), nullptr);
            }
        }
        std::cerr << "[rsp-sshd] execl failed: " << strerror(errno) << '\n';
        ::_exit(1);
    }

    // Parent: fds[1] belongs to the child; fds[0] is our side.
    ::close(fds[1]);
    log("Spawned sshd -i (pid=" + std::to_string(pid) + ")");

    TCPConnectionResult result;
    result.connection = std::make_shared<SshdConnection>(fds[0], pid);
    return registerConnectedSocket(message, std::move(result), *socketId, "sshd", asyncData, shareSocket);
}

}  // namespace rsp::resource_service

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

namespace {
std::atomic<bool> gStopRequested{false};

void signalHandler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        gStopRequested.store(true);
    } else if (sig == SIGCHLD) {
        while (::waitpid(-1, nullptr, WNOHANG) > 0) {}
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    const std::string configPath = (argc > 1) ? argv[1] : "/etc/rsp-sshd/rsp_sshd.conf.json";

    rsp::resource_service::SshdConfig cfg;
    try {
        cfg = rsp::resource_service::loadSshdConfig(configPath);
    } catch (const std::exception& ex) {
        std::cerr << "[rsp-sshd] Config error: " << ex.what() << '\n';
        return 1;
    }

    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGCHLD, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    auto rs = rsp::resource_service::SshdResourceService::create(cfg);

    log("Connecting to RSP transport: " + cfg.rspTransport);
    const auto connId = rs->connectToResourceManager(
        cfg.rspTransport, rsp::message_queue::kAsciiHandshakeEncoding);
    if (connId == rsp::GUID{}) {
        log("Failed to connect to resource manager: " + cfg.rspTransport);
        return 1;
    }
    log("Connected to RSP transport: " + cfg.rspTransport);
    log("Node ID: " + rs->nodeId().toString());
    log("Registered as ResourceService — ready to accept SSH connections");

    // If a hostname is configured, register with the name service in the background.
    // This runs in a separate thread so sshd can immediately accept connections
    // while the second RM connection (and its authn handshake) completes asynchronously.
    std::shared_ptr<rsp::client::RSPClient> nsClient;
    std::thread nsThread;
    if (!cfg.nsHostname.empty()) {
        nsClient = rsp::client::RSPClient::create();
        const std::string transport = cfg.rspTransport;
        const std::string hostname  = cfg.nsHostname;
        const rsp::NodeID sshdNodeId = rs->nodeId();

        nsThread = std::thread([nsClient, transport, hostname, sshdNodeId]() {
            try {
                const auto nsConnId = nsClient->connectToResourceManager(
                    transport, rsp::message_queue::kAsciiHandshakeEncoding);
                if (!nsConnId.has_value()) {
                    std::cerr << "[rsp-sshd] Warning: could not open NS discovery connection\n";
                    return;
                }
                const auto rmNodeIdOpt = nsClient->peerNodeID(*nsConnId);
                if (!rmNodeIdOpt.has_value()) {
                    std::cerr << "[rsp-sshd] Warning: could not get RM node ID for NS\n";
                    return;
                }
                std::cerr << "[rsp-sshd] Querying RM " << rmNodeIdOpt->toString()
                          << " for name service nodes\n";
                const auto queryResult = nsClient->resourceList(*rmNodeIdOpt, "");
                if (!queryResult.has_value()) {
                    std::cerr << "[rsp-sshd] Warning: resource list query timed out\n";
                    return;
                }
                std::cerr << "[rsp-sshd] Resource list returned "
                          << queryResult->services.size() << " service(s)\n";
                int nsCount = 0;
                for (const auto& svc : queryResult->services) {
                    bool isNS = false;
                    for (const auto& url : svc.acceptedTypeUrls) {
                        if (url == "type.rsp/rsp.proto.NameCreateRequest") {
                            isNS = true;
                            break;
                        }
                    }
                    if (!isNS) continue;
                    const rsp::NodeID nsNodeId = svc.nodeId;
                    if (nsNodeId == rsp::NodeID{}) continue;

                    const auto reply = nsClient->registerNameWithRefresh(
                        nsNodeId,
                        hostname,
                        sshdNodeId,
                        rsp::resource_service::kSshdNameType,
                        sshdNodeId);
                    if (reply.has_value()) {
                        std::cerr << "[rsp-sshd] Registered hostname '" << hostname
                                  << "' with NS node " << nsNodeId.toString() << '\n';
                        ++nsCount;
                    } else {
                        std::cerr << "[rsp-sshd] Warning: nameCreate failed for NS "
                                  << nsNodeId.toString() << '\n';
                    }
                }
                if (nsCount == 0) {
                    std::cerr << "[rsp-sshd] Warning: no name servers found; hostname not registered\n";
                }
            } catch (const std::exception& ex) {
                std::cerr << "[rsp-sshd] Warning: NS registration failed: " << ex.what() << '\n';
            }
        });
    }

    while (!gStopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (nsThread.joinable()) {
        nsThread.join();
    }

    std::cerr << "[rsp-sshd] Shutting down\n";
    return 0;
}
