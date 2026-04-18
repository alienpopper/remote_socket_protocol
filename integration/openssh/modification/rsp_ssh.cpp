// rsp_ssh.cpp - RSP SSH ProxyCommand client
//
// Connects to an RSP resource manager as a client, opens a socket to an sshd
// exposed via RSP, then bridges stdin/stdout to that socket so that the ssh
// client can communicate transparently.
//
// Usage with ssh:
//   ssh -o ProxyCommand='rsp_ssh /etc/rsp-ssh/rsp_ssh.conf.json' user@host
//
// The host/user arguments to ssh are handled by ssh itself; rsp_ssh only
// needs the config file that specifies which RSP node and port to connect to.
//
// Default config path: /etc/rsp-ssh/rsp_ssh.conf.json

#define RSPCLIENT_STATIC

#include "client/cpp/rsp_client.hpp"
#include "common/endorsement/well_known_endorsements.h"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "os/os_socket.hpp"
#include "resource_service/sshd/resource_service_sshd.hpp"
#include "third_party/json/single_include/nlohmann/json.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

using nlohmann::json;

namespace {

std::atomic<bool> gDone{false};
std::atomic<int>  gSocketFd{-1};

void signalHandler(int) {
    gDone.store(true);
    // Interrupt blocking reads in the pump threads by closing the socket.
    // This makes read(socketFd) return an error, which causes both pump
    // threads to notice gDone and exit.
    const int fd = gSocketFd.exchange(-1);
    if (fd != -1) {
        ::shutdown(fd, SHUT_RDWR);
    }
}

struct Config {
    std::string rspTransport;
    std::string resourceServiceNodeId;   // direct node ID (mutually exclusive with resourceServiceName)
    std::string resourceServiceName;     // name to look up in NS (mutually exclusive with resourceServiceNodeId)
    std::string endorsementNodeId;   // optional
    std::string hostPort = "127.0.0.1:22";
    uint32_t    connectTimeoutMs = 5000;
    uint32_t    connectRetries   = 0;
};

Config loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    json j;
    file >> j;

    Config cfg;
    cfg.rspTransport = j.at("rsp_transport").get<std::string>();

    if (j.contains("resource_service_node_id")) {
        cfg.resourceServiceNodeId = j["resource_service_node_id"].get<std::string>();
    }
    if (j.contains("resource_service_name")) {
        cfg.resourceServiceName = j["resource_service_name"].get<std::string>();
    }
    if (cfg.resourceServiceNodeId.empty() && cfg.resourceServiceName.empty()) {
        throw std::runtime_error("Config must specify resource_service_node_id or resource_service_name");
    }

    if (j.contains("endorsement_node_id")) {
        cfg.endorsementNodeId = j["endorsement_node_id"].get<std::string>();
    }
    if (j.contains("host_port")) {
        cfg.hostPort = j["host_port"].get<std::string>();
    }
    if (j.contains("connect_timeout_ms")) {
        cfg.connectTimeoutMs = j["connect_timeout_ms"].get<uint32_t>();
    }
    if (j.contains("connect_retries")) {
        cfg.connectRetries = j["connect_retries"].get<uint32_t>();
    }

    return cfg;
}

void log(const std::string& message) {
    // ProxyCommand: stderr is passed through to the terminal; stdout is the
    // SSH data channel, so all diagnostics must go to stderr.
    std::cerr << "[rsp-ssh] " << message << '\n';
}

bool acquireEndorsement(rsp::client::RSPClient& client,
                        const rsp::NodeID& esNodeId,
                        const rsp::GUID& etype,
                        const rsp::GUID& evalue,
                        const std::string& label) {
    // Convert GUID to Buffer for the endorsement value
    const uint64_t high = evalue.high();
    const uint64_t low = evalue.low();
    uint8_t bytes[16];
    std::memcpy(bytes, &high, 8);
    std::memcpy(bytes + 8, &low, 8);
    const rsp::Buffer evalueBuffer(bytes, 16);

    for (int attempt = 0; attempt < 3; ++attempt) {
        auto reply = client.beginEndorsementRequest(esNodeId, etype, evalueBuffer);
        if (reply.has_value() && reply->status() == rsp::proto::ENDORSEMENT_SUCCESS) {
            return true;
        }
    }
    log("Failed to acquire endorsement: " + label);
    return false;
}

// Pump bytes from srcFd to dstFd until EOF, error, or gDone is set.
void pumpFd(int srcFd, int dstFd, const std::string& label) {
    constexpr std::size_t kBufSize = 16 * 1024;
    uint8_t buf[kBufSize];

    while (!gDone.load()) {
        const ssize_t nr = ::read(srcFd, buf, kBufSize);
        if (nr <= 0) {
            break;   // EOF or error
        }

        std::size_t written = 0;
        while (written < static_cast<std::size_t>(nr) && !gDone.load()) {
            const ssize_t nw = ::write(dstFd, buf + written,
                                       static_cast<std::size_t>(nr) - written);
            if (nw <= 0) {
                gDone.store(true);
                return;
            }
            written += static_cast<std::size_t>(nw);
        }
    }

    gDone.store(true);
    // When stdin→socket finishes: signal EOF to sshd so the socket→stdout
    // thread sees the connection close.
    // When socket→stdout finishes: close/shutdown the socket so stdinToSocket
    // unblocks (gSocketFd.exchange ensures we only close once).
    const int sockFd = gSocketFd.exchange(-1);
    if (sockFd != -1) {
        ::shutdown(sockFd, SHUT_RDWR);
    }
    (void)label;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string configPath = (argc > 1) ? argv[1] : "/etc/rsp-ssh/rsp_ssh.conf.json";

    Config cfg;
    try {
        cfg = loadConfig(configPath);
    } catch (const std::exception& ex) {
        log(std::string("Config error: ") + ex.what());
        return 1;
    }

    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

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
            !acquireEndorsement(*client, esNodeId, ETYPE_ROLE, EVALUE_ROLE_CLIENT, "client role")) {
            return 1;
        }
        log("Endorsements acquired");
    }

    // Resolve the sshd node ID — either from config or via name lookup.
    rsp::NodeID rsNodeId;
    if (!cfg.resourceServiceNodeId.empty()) {
        rsNodeId = rsp::NodeID{cfg.resourceServiceNodeId};
    } else {
        // Discover NS nodes via RM, then look up the configured service name.
        const auto rmNodeIdOpt = client->peerNodeID(connId);
        if (!rmNodeIdOpt.has_value()) {
            log("Failed to get RM node ID for name lookup");
            return 1;
        }
        const auto queryResult = client->resourceList(*rmNodeIdOpt, "");
        if (!queryResult.has_value()) {
            log("Resource list query failed");
            return 1;
        }
        bool resolved = false;
        for (const auto& svc : queryResult->services()) {
            bool isNS = false;
            for (const auto& url : svc.schema().accepted_type_urls()) {
                if (url == "type.rsp/rsp.proto.NameCreateRequest") {
                    isNS = true;
                    break;
                }
            }
            if (!isNS) continue;

            const auto nsNodeIdOpt = rsp::nodeIdFromSourceField(svc.node_id());
            if (!nsNodeIdOpt.has_value()) continue;

            log("Querying NS " + nsNodeIdOpt->toString() + " for name '" + cfg.resourceServiceName + "'");
            const auto nameReply = client->nameQuery(
                *nsNodeIdOpt,
                cfg.resourceServiceName,
                std::nullopt,
                rsp::resource_service::kSshdNameType);
            if (!nameReply.has_value() || nameReply->records().empty()) continue;

            const auto ownerNodeIdOpt = rsp::nodeIdFromSourceField(nameReply->records(0).owner());
            if (!ownerNodeIdOpt.has_value()) continue;

            rsNodeId = *ownerNodeIdOpt;
            log("Resolved '" + cfg.resourceServiceName + "' to node " + rsNodeId.toString());
            resolved = true;
            break;
        }
        if (!resolved) {
            log("Name lookup failed for '" + cfg.resourceServiceName + "'");
            return 1;
        }
    }

    const auto sockFd = client->connectSshdSocket(rsNodeId, cfg.connectTimeoutMs);
    if (!sockFd.has_value() || !rsp::os::isValidSocket(*sockFd)) {
        log("Failed to connect RSP sshd socket");
        return 1;
    }
    log("RSP sshd socket connected");

    const int fd = static_cast<int>(*sockFd);
    gSocketFd.store(fd);

    // Two threads bridge the two directions concurrently.
    // Either direction finishing sets gDone and unblocks the other.
    std::thread stdinToSocket([fd]() {
        pumpFd(STDIN_FILENO, fd, "stdin→socket");
    });

    std::thread socketToStdout([fd]() {
        pumpFd(fd, STDOUT_FILENO, "socket→stdout");
    });

    stdinToSocket.join();
    socketToStdout.join();

    // Close the socket if not already closed by the signal handler.
    const int remainingFd = gSocketFd.exchange(-1);
    if (remainingFd != -1) {
        rsp::os::closeSocket(static_cast<rsp::os::SocketHandle>(remainingFd));
    }
    log("Connection closed");
    return 0;
}
