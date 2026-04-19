// resource_service_httpd_main.cpp — rsp_httpd standalone binary entry point
//
// See resource_service_httpd.cpp for implementation details.

#include "resource_service/httpd/resource_service_httpd.hpp"

#include "common/message_queue/mq_ascii_handshake.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> gStopRequested{false};

void signalHandler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        gStopRequested.store(true);
    }
}

void log(const std::string& msg) {
    std::cerr << "[rsp-httpd] " << msg << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string configPath = (argc > 1) ? argv[1] : "/etc/rsp-httpd/rsp_httpd.conf.json";

    rsp::resource_service::HttpdConfig cfg;
    try {
        cfg = rsp::resource_service::loadHttpdConfig(configPath);
    } catch (const std::exception& ex) {
        std::cerr << "[rsp-httpd] Config error: " << ex.what() << '\n';
        return 1;
    }

    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    auto rs = rsp::resource_service::HttpdResourceService::create(cfg);

    log("Connecting to RSP transport: " + cfg.rspTransport);
    const auto connId = rs->connectToResourceManager(
        cfg.rspTransport, rsp::message_queue::kAsciiHandshakeEncoding);
    if (connId == rsp::GUID{}) {
        log("Failed to connect to resource manager: " + cfg.rspTransport);
        return 1;
    }
    log("Connected to RSP transport: " + cfg.rspTransport);
    log("Node ID: " + rs->nodeId().toString());
    log("Built-in HTTP server port: " + std::to_string(rs->builtinServerPort()));
    log("Registered as HttpdResourceService — ready to accept HTTP connections");

    while (!gStopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    log("Shutting down");
    return 0;
}
