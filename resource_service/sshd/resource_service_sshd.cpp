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

#include "common/keypair.hpp"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/transport/transport.hpp"
#include "third_party/json/single_include/nlohmann/json.hpp"

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
    return std::shared_ptr<SshdResourceService>(
        new SshdResourceService(rsp::KeyPair::generateP256(), cfg));
}

SshdResourceService::SshdResourceService(rsp::KeyPair keyPair, const SshdConfig& cfg)
    : BsdSocketsResourceService(std::move(keyPair)), cfg_(cfg) {}

bool SshdResourceService::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_connect_sshd()) {
        return handleConnectSshd(message);
    }

    // Reject bsd_sockets-specific messages that sshd does not support.
    if (message.has_connect_tcp_request() || message.has_listen_tcp_request() || message.has_accept_tcp()) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR,
                                           "UNIMPLEMENTED: rsp_sshd does not support this request"));
    }

    // Delegate socket_send, socket_recv, socket_close to the parent.
    return BsdSocketsResourceService::handleNodeSpecificMessage(message);
}

bool SshdResourceService::handleConnectSshd(const rsp::proto::RSPMessage& message) {
    const auto& request = message.connect_sshd();

    if (!request.has_socket_number()) {
        return send(makeSocketReplyMessage(message, rsp::proto::INVALID_FLAGS, "socket_number is required"));
    }

    const auto socketId = fromProtoSocketId(request.socket_number());
    if (!socketId.has_value()) {
        return send(makeSocketReplyMessage(message, rsp::proto::INVALID_FLAGS, "invalid socket_number"));
    }

    const bool asyncData = request.has_async_data() && request.async_data();
    const bool shareSocket = request.has_share_socket() && request.share_socket();

    if (shareSocket) {
        if (asyncData) {
            return send(makeSocketReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_socket cannot be combined with async_data",
                                               &*socketId));
        }
        if (request.has_use_socket() && request.use_socket()) {
            return send(makeSocketReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_socket cannot be combined with use_socket",
                                               &*socketId));
        }
    }

    // Create a socketpair and fork sshd -i on the child end.
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        log("socketpair failed: " + std::string(strerror(errno)));
        return send(makeSocketReplyMessage(message, rsp::proto::CONNECT_REFUSED,
                                           "socketpair failed", &*socketId));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        log("fork failed: " + std::string(strerror(errno)));
        ::close(fds[0]);
        ::close(fds[1]);
        return send(makeSocketReplyMessage(message, rsp::proto::CONNECT_REFUSED,
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
    return registerConnectedSocket(message, std::move(result), *socketId, asyncData, shareSocket);
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

    const auto connId = rs->connectToResourceManager(
        cfg.rspTransport, rsp::message_queue::kAsciiHandshakeEncoding);
    if (connId == rsp::GUID{}) {
        std::cerr << "[rsp-sshd] Failed to connect to resource manager: " << cfg.rspTransport << '\n';
        return 1;
    }
    std::cerr << "[rsp-sshd] Connected to RSP transport: " << cfg.rspTransport << '\n';
    std::cerr << "[rsp-sshd] Node ID: " << rs->nodeId().toString() << '\n';
    std::cerr << "[rsp-sshd] Registered as ResourceService — ready to accept SSH connections\n";

    while (!gStopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::cerr << "[rsp-sshd] Shutting down\n";
    return 0;
}
