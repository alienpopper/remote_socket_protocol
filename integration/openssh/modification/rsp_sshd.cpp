// rsp_sshd.cpp - OpenSSH server forwarder over RSP
//
// Connects to an RSP resource manager as a resource service, listens for
// incoming RSP connections, and for each connection spawns `sshd -i` in
// inetd mode, with the RSP-bridged socket fd inherited as stdin/stdout.
//
// Compatible with systemd: logs to stderr (captured by journald), handles
// SIGTERM/SIGCHLD cleanly.
//
// Usage:
//   rsp_sshd [/path/to/rsp_sshd.conf.json]
//
// Default config path: /etc/rsp-sshd/rsp_sshd.conf.json

#define RSPCLIENT_STATIC

#include "client/cpp/rsp_client.hpp"
#include "common/endorsement/well_known_endorsements.h"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "os/os_socket.hpp"
#include "third_party/json/single_include/nlohmann/json.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using nlohmann::json;

namespace {

std::atomic<bool> gStopRequested{false};

void signalHandler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        gStopRequested.store(true);
    } else if (sig == SIGCHLD) {
        // Reap any finished sshd children without blocking.
        while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    }
}

struct Config {
    std::string rspTransport;
    std::string resourceServiceNodeId;
    std::string endorsementNodeId;   // optional
    std::string hostPort = "127.0.0.1:22";
    std::string sshdPath = "/usr/sbin/sshd";
    std::string sshdConfig;          // optional; empty = sshd default
    bool        sshdDebug = false;
};

Config loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    json j;
    file >> j;

    Config cfg;
    cfg.rspTransport          = j.at("rsp_transport").get<std::string>();
    cfg.resourceServiceNodeId = j.at("resource_service_node_id").get<std::string>();

    if (j.contains("endorsement_node_id")) {
        cfg.endorsementNodeId = j["endorsement_node_id"].get<std::string>();
    }
    if (j.contains("host_port")) {
        cfg.hostPort = j["host_port"].get<std::string>();
    }
    if (j.contains("sshd_path")) {
        cfg.sshdPath = j["sshd_path"].get<std::string>();
    }
    if (j.contains("sshd_config")) {
        cfg.sshdConfig = j["sshd_config"].get<std::string>();
    }
    if (j.contains("sshd_debug")) {
        cfg.sshdDebug = j["sshd_debug"].get<bool>();
    }

    return cfg;
}

void log(const std::string& message) {
    std::cerr << "[rsp-sshd] " << message << '\n';
}

bool acquireEndorsement(rsp::client::RSPClient& client,
                        const rsp::NodeID& esNodeId,
                        const rsp::GUID& etype,
                        const rsp::GUID& evalue,
                        const std::string& label) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto reply = client.beginEndorsementRequest(esNodeId, etype, evalue);
        if (reply.has_value() && reply->status() == rsp::proto::ENDORSEMENT_SUCCESS) {
            return true;
        }
    }
    log("Failed to acquire endorsement: " + label);
    return false;
}

// Fork and exec sshd -i with connFd as its stdin and stdout.
void spawnSshd(rsp::os::SocketHandle connFd, const Config& cfg) {
    const pid_t pid = fork();
    if (pid < 0) {
        log("fork() failed: " + std::string(strerror(errno)));
        rsp::os::closeSocket(connFd);
        return;
    }

    if (pid == 0) {
        // Child: wire the socket fd to stdin and stdout, then exec sshd.
        if (dup2(static_cast<int>(connFd), STDIN_FILENO) < 0 ||
            dup2(static_cast<int>(connFd), STDOUT_FILENO) < 0) {
            std::cerr << "[rsp-sshd] dup2 failed: " << strerror(errno) << '\n';
            _exit(1);
        }
        // Close the original fd if it's not stdin/stdout.
        if (connFd != STDIN_FILENO && connFd != STDOUT_FILENO) {
            close(static_cast<int>(connFd));
        }

        if (cfg.sshdDebug) {
            if (cfg.sshdConfig.empty()) {
                execl(cfg.sshdPath.c_str(), cfg.sshdPath.c_str(), "-i", "-d", nullptr);
            } else {
                execl(cfg.sshdPath.c_str(), cfg.sshdPath.c_str(), "-i", "-d",
                      "-f", cfg.sshdConfig.c_str(), nullptr);
            }
        } else {
            if (cfg.sshdConfig.empty()) {
                execl(cfg.sshdPath.c_str(), cfg.sshdPath.c_str(), "-i", nullptr);
            } else {
                execl(cfg.sshdPath.c_str(), cfg.sshdPath.c_str(), "-i",
                      "-f", cfg.sshdConfig.c_str(), nullptr);
            }
        }

        std::cerr << "[rsp-sshd] execl failed: " << strerror(errno) << '\n';
        _exit(1);
    }

    // Parent: close our copy of the socket; the child owns it now.
    rsp::os::closeSocket(connFd);
    log("Spawned sshd -i (pid=" + std::to_string(pid) + ")");
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string configPath = (argc > 1) ? argv[1] : "/etc/rsp-sshd/rsp_sshd.conf.json";

    Config cfg;
    try {
        cfg = loadConfig(configPath);
    } catch (const std::exception& ex) {
        log(std::string("Config error: ") + ex.what());
        return 1;
    }

    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGCHLD, signalHandler);

    auto client = rsp::client::RSPClient::create();

    const auto connId = client->connectToResourceManager(
        cfg.rspTransport, rsp::message_queue::kAsciiHandshakeEncoding);
    if (connId == rsp::GUID{}) {
        log("Failed to connect to resource manager: " + cfg.rspTransport);
        return 1;
    }
    log("Connected to RSP transport: " + cfg.rspTransport);

    if (!cfg.endorsementNodeId.empty()) {
        const rsp::NodeID esNodeId{cfg.endorsementNodeId};

        if (!client->ping(esNodeId)) {
            log("Endorsement service unreachable: " + cfg.endorsementNodeId);
            return 1;
        }
        if (!acquireEndorsement(*client, esNodeId, ETYPE_ACCESS, EVALUE_ACCESS_NETWORK, "network access") ||
            !acquireEndorsement(*client, esNodeId, ETYPE_ROLE, EVALUE_ROLE_RESOURCE_SERVICE, "resource service role")) {
            return 1;
        }
        log("Endorsements acquired");
    }

    const rsp::NodeID rsNodeId{cfg.resourceServiceNodeId};
    const auto listenerFd = client->listenTCPSocket(rsNodeId, cfg.hostPort);
    if (!listenerFd.has_value() || !rsp::os::isValidSocket(*listenerFd)) {
        log("Failed to open RSP listen socket on " + cfg.hostPort);
        return 1;
    }
    log("Listening on RSP host_port=" + cfg.hostPort + " via RS node=" + cfg.resourceServiceNodeId);

    while (!gStopRequested.load()) {
        const rsp::os::SocketHandle connFd = rsp::os::acceptSocket(*listenerFd);
        if (!rsp::os::isValidSocket(connFd)) {
            if (gStopRequested.load()) break;
            // acceptSocket can return invalid on EINTR (signal); just retry.
            continue;
        }
        log("Incoming RSP connection; spawning sshd -i");
        spawnSshd(connFd, cfg);
    }

    log("Shutting down");
    rsp::os::closeSocket(*listenerFd);
    return 0;
}
