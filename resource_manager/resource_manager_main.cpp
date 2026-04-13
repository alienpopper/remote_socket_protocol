#include "resource_manager/resource_manager.hpp"

#include "common/transport/transport_tcp.hpp"

#include <condition_variable>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

static std::mutex gShutdownMutex;
static std::condition_variable gShutdownCv;
static bool gShouldShutdown = false;

static void signalHandler(int) {
    std::lock_guard<std::mutex> lock(gShutdownMutex);
    gShouldShutdown = true;
    gShutdownCv.notify_all();
}

int main(int argc, char** argv) {
    const std::string listenEndpoint = argc > 1 ? argv[1] : "0.0.0.0:7000";

    auto transport = std::make_shared<rsp::transport::TcpTransport>();
    if (!transport->listen(listenEndpoint)) {
        std::cerr << "resource_manager: failed to listen on " << listenEndpoint << '\n';
        return 1;
    }
    std::cout << "resource_manager: listening on " << listenEndpoint << std::endl;

    std::vector<rsp::transport::ListeningTransportHandle> clientTransports;
    clientTransports.push_back(transport);

    rsp::resource_manager::ResourceManager resourceManager(std::move(clientTransports));

    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT, signalHandler);

    std::unique_lock<std::mutex> lock(gShutdownMutex);
    gShutdownCv.wait(lock, []() { return gShouldShutdown; });

    return 0;
}