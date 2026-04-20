#include "common/transport/transport_tcp.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool waitForCondition(const std::function<bool()>& condition,
                      std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

// Verifies that a basic connect/disconnect cycle works correctly.
void testConnectAndDisconnect() {
    auto server = std::make_shared<rsp::transport::TcpTransport>();
    require(server->listen("127.0.0.1:0"), "server should begin listening on a random port");

    const uint16_t port = server->listenedPort();
    require(port != 0, "server should have a non-zero listening port");

    auto client = std::make_shared<rsp::transport::TcpTransport>();
    const auto conn = client->connect(std::string("127.0.0.1:") + std::to_string(port));
    require(conn != nullptr, "client should connect to the server");
    require(client->connection() == conn, "client should expose the active connection");

    client->stop();
    server->stop();
}

// Verifies that reconnect() with enabled=false makes exactly one attempt and
// returns nullptr when the server is not listening.
void testReconnectDisabledSingleAttempt() {
    auto server = std::make_shared<rsp::transport::TcpTransport>();
    require(server->listen("127.0.0.1:0"), "server should begin listening");
    const uint16_t port = server->listenedPort();

    auto client = std::make_shared<rsp::transport::TcpTransport>();
    const auto conn = client->connect(std::string("127.0.0.1:") + std::to_string(port));
    require(conn != nullptr, "initial connect should succeed");

    server->stop();

    rsp::transport::ReconnectConfig config;
    config.enabled = false;
    client->setReconnectConfig(config);

    const auto result = client->reconnect();
    require(result == nullptr, "reconnect with enabled=false should return nullptr when server is gone");

    client->stop();
}

// Verifies that reconnect() returns nullptr after maxAttempts retries have failed.
void testReconnectMaxAttempts() {
    auto server = std::make_shared<rsp::transport::TcpTransport>();
    require(server->listen("127.0.0.1:0"), "server should begin listening");
    const uint16_t port = server->listenedPort();

    auto client = std::make_shared<rsp::transport::TcpTransport>();
    const auto conn = client->connect(std::string("127.0.0.1:") + std::to_string(port));
    require(conn != nullptr, "initial connect should succeed");

    server->stop();

    rsp::transport::ReconnectConfig config;
    config.enabled = true;
    config.initialIntervalMs = 1;
    config.maxIntervalMs = 5;
    config.backoffMultiplier = 1.0;
    config.maxAttempts = 3;
    client->setReconnectConfig(config);

    const auto result = client->reconnect();
    require(result == nullptr, "reconnect should return nullptr after maxAttempts exhausted");

    client->stop();
}

// Verifies that reconnect() with backoff eventually succeeds when the server
// comes back online after a short delay.
void testReconnectWithBackoffSucceeds() {
    auto server = std::make_shared<rsp::transport::TcpTransport>();
    require(server->listen("127.0.0.1:0"), "server should begin listening");
    const uint16_t port = server->listenedPort();

    auto client = std::make_shared<rsp::transport::TcpTransport>();
    const auto conn = client->connect(std::string("127.0.0.1:") + std::to_string(port));
    require(conn != nullptr, "initial connect should succeed");

    server->stop();

    rsp::transport::ReconnectConfig config;
    config.enabled = true;
    config.initialIntervalMs = 10;
    config.maxIntervalMs = 50;
    config.backoffMultiplier = 2.0;
    config.maxAttempts = 0;
    client->setReconnectConfig(config);

    // Start a new server on the same port after a short delay.
    std::atomic<bool> serverStarted{false};
    auto newServer = std::make_shared<rsp::transport::TcpTransport>();
    std::thread serverThread([&newServer, &serverStarted, port]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        newServer->listen(std::string("127.0.0.1:") + std::to_string(port));
        serverStarted.store(true);
    });

    const auto result = client->reconnect();
    serverThread.join();
    newServer->stop();
    client->stop();

    require(serverStarted.load(), "server thread should have started before reconnect returned");
    require(result != nullptr, "reconnect with backoff should succeed when the server comes back online");
}

// Verifies that stop() interrupts a reconnect() that is waiting between retries.
void testStopInterruptsReconnect() {
    auto server = std::make_shared<rsp::transport::TcpTransport>();
    require(server->listen("127.0.0.1:0"), "server should begin listening");
    const uint16_t port = server->listenedPort();

    auto client = std::make_shared<rsp::transport::TcpTransport>();
    const auto conn = client->connect(std::string("127.0.0.1:") + std::to_string(port));
    require(conn != nullptr, "initial connect should succeed");

    server->stop();

    // Long intervals so the reconnect loop will be sleeping when we call stop().
    rsp::transport::ReconnectConfig config;
    config.enabled = true;
    config.initialIntervalMs = 5000;
    config.maxIntervalMs = 5000;
    config.backoffMultiplier = 1.0;
    config.maxAttempts = 0;
    client->setReconnectConfig(config);

    std::atomic<bool> reconnectReturned{false};
    std::thread reconnectThread([&client, &reconnectReturned]() {
        client->reconnect();
        reconnectReturned.store(true);
    });

    // Give the reconnect loop a moment to enter its first sleep.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client->stop();

    const bool finished = waitForCondition([&reconnectReturned] {
        return reconnectReturned.load();
    }, std::chrono::seconds(3));

    reconnectThread.join();
    require(finished, "stop() should interrupt the reconnect() sleep and cause it to return promptly");
}

}  // namespace

int main() {
    try {
        std::cout << "testConnectAndDisconnect" << std::endl;
        testConnectAndDisconnect();

        std::cout << "testReconnectDisabledSingleAttempt" << std::endl;
        testReconnectDisabledSingleAttempt();

        std::cout << "testReconnectMaxAttempts" << std::endl;
        testReconnectMaxAttempts();

        std::cout << "testReconnectWithBackoffSucceeds" << std::endl;
        testReconnectWithBackoffSucceeds();

        std::cout << "testStopInterruptsReconnect" << std::endl;
        testStopInterruptsReconnect();

        std::cout << "all transport tcp tests passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test failed: " << e.what() << std::endl;
        return 1;
    }
}
