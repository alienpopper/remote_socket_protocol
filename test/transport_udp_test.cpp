#include "common/transport/transport_udp.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
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

// Verifies that a client can connect to a listening UdpTransport, send a
// datagram, and receive a reply echoed back by the server-side connection.
void testUdpClientServerRoundTrip() {
    auto serverTransport = std::make_shared<rsp::transport::UdpTransport>();

    std::promise<rsp::transport::ConnectionHandle> serverConnPromise;
    std::future<rsp::transport::ConnectionHandle> serverConnFuture =
        serverConnPromise.get_future();

    serverTransport->setNewConnectionCallback(
        [&serverConnPromise](const rsp::transport::ConnectionHandle& conn) {
            try {
                serverConnPromise.set_value(conn);
            } catch (...) {
                serverConnPromise.set_exception(std::current_exception());
            }
        });

    require(serverTransport->listen("127.0.0.1:0"),
            "UDP transport should bind to an ephemeral port on 127.0.0.1");

    const uint16_t port = serverTransport->listenedPort();
    require(port != 0, "listenedPort() should return a non-zero ephemeral port");

    auto clientTransport = std::make_shared<rsp::transport::UdpTransport>();
    const std::string serverEndpoint = "127.0.0.1:" + std::to_string(port);
    rsp::transport::ConnectionHandle clientConn = clientTransport->connect(serverEndpoint);
    require(clientConn != nullptr, "client should create a connected UDP socket");

    // Send from client to server.
    const std::string payload = "hello-udp";
    rsp::Buffer sendBuf(reinterpret_cast<const uint8_t*>(payload.data()),
                        static_cast<uint32_t>(payload.size()));
    const int sentBytes = clientConn->send(sendBuf);
    require(sentBytes == static_cast<int>(payload.size()),
            "client should send all payload bytes");

    // Wait for the server-side connection to be created.
    require(serverConnFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "server should receive first datagram and create a server-side connection");

    rsp::transport::ConnectionHandle serverConn = serverConnFuture.get();
    require(serverConn != nullptr, "server-side connection should be non-null");

    // Server receives the datagram.
    rsp::Buffer recvBuf;
    const int recvBytes = serverConn->recv(recvBuf);
    require(recvBytes == static_cast<int>(payload.size()),
            "server should receive exactly the payload bytes");
    require(std::memcmp(recvBuf.data(), payload.data(), payload.size()) == 0,
            "server should receive the correct payload data");

    // Server echoes the datagram back to the client.
    const int echoBytes = serverConn->send(recvBuf);
    require(echoBytes == recvBytes, "server should echo all bytes back to client");

    // Client receives the echo.
    rsp::Buffer echoBuf;
    const int echoRecvBytes = clientConn->recv(echoBuf);
    require(echoRecvBytes == static_cast<int>(payload.size()),
            "client should receive the echo datagram");
    require(std::memcmp(echoBuf.data(), payload.data(), payload.size()) == 0,
            "client should receive the correct echoed data");

    clientTransport->stop();
    serverTransport->stop();
}

// Verifies that two independent clients are demuxed to distinct server-side
// connections, and that datagrams are delivered to the correct connection.
void testUdpMultiplePeersDemux() {
    auto serverTransport = std::make_shared<rsp::transport::UdpTransport>();

    std::mutex connsMutex;
    std::vector<rsp::transport::ConnectionHandle> serverConns;

    serverTransport->setNewConnectionCallback(
        [&connsMutex, &serverConns](const rsp::transport::ConnectionHandle& conn) {
            std::lock_guard<std::mutex> lock(connsMutex);
            serverConns.push_back(conn);
        });

    require(serverTransport->listen("127.0.0.1:0"),
            "UDP transport should bind for multi-peer test");

    const uint16_t port = serverTransport->listenedPort();
    const std::string serverEndpoint = "127.0.0.1:" + std::to_string(port);

    auto clientA = std::make_shared<rsp::transport::UdpTransport>();
    auto clientB = std::make_shared<rsp::transport::UdpTransport>();

    rsp::transport::ConnectionHandle connA = clientA->connect(serverEndpoint);
    rsp::transport::ConnectionHandle connB = clientB->connect(serverEndpoint);

    require(connA != nullptr, "client A should create a connected UDP socket");
    require(connB != nullptr, "client B should create a connected UDP socket");

    const std::string payloadA = "from-client-a";
    const std::string payloadB = "from-client-b";

    rsp::Buffer bufA(reinterpret_cast<const uint8_t*>(payloadA.data()),
                     static_cast<uint32_t>(payloadA.size()));
    rsp::Buffer bufB(reinterpret_cast<const uint8_t*>(payloadB.data()),
                     static_cast<uint32_t>(payloadB.size()));

    connA->send(bufA);
    connB->send(bufB);

    // Wait for both server-side connections to be created.
    require(waitForCondition([&connsMutex, &serverConns]() {
                std::lock_guard<std::mutex> lock(connsMutex);
                return serverConns.size() >= 2;
            }),
            "server should demux two clients into two distinct server-side connections");

    // Receive from each server-side connection and verify the correct payload.
    // We don't know which server connection maps to which client, so we receive
    // from both and verify that each got one of the two expected payloads.
    rsp::transport::ConnectionHandle sc0, sc1;
    {
        std::lock_guard<std::mutex> lock(connsMutex);
        sc0 = serverConns[0];
        sc1 = serverConns[1];
    }

    rsp::Buffer r0, r1;
    const int rb0 = sc0->recv(r0);
    const int rb1 = sc1->recv(r1);

    require(rb0 > 0, "first server connection should receive a datagram");
    require(rb1 > 0, "second server connection should receive a datagram");

    const std::string got0(reinterpret_cast<const char*>(r0.data()),
                            static_cast<size_t>(rb0));
    const std::string got1(reinterpret_cast<const char*>(r1.data()),
                            static_cast<size_t>(rb1));

    const bool ordered = (got0 == payloadA && got1 == payloadB);
    const bool swapped = (got0 == payloadB && got1 == payloadA);
    require(ordered || swapped,
            "each server connection should receive exactly one client's payload");

    clientA->stop();
    clientB->stop();
    serverTransport->stop();
}

// Verifies that a single client can send multiple datagrams (a batch) to the
// server and receive them all in order on the server-side connection.
void testUdpMultipleDatgramBatch() {
    auto serverTransport = std::make_shared<rsp::transport::UdpTransport>();

    std::promise<rsp::transport::ConnectionHandle> serverConnPromise;
    std::future<rsp::transport::ConnectionHandle> serverConnFuture =
        serverConnPromise.get_future();

    serverTransport->setNewConnectionCallback(
        [&serverConnPromise](const rsp::transport::ConnectionHandle& conn) {
            try {
                serverConnPromise.set_value(conn);
            } catch (...) {
                serverConnPromise.set_exception(std::current_exception());
            }
        });

    require(serverTransport->listen("127.0.0.1:0"),
            "UDP transport should bind for batch test");

    const uint16_t port = serverTransport->listenedPort();
    auto clientTransport = std::make_shared<rsp::transport::UdpTransport>();
    rsp::transport::ConnectionHandle clientConn =
        clientTransport->connect("127.0.0.1:" + std::to_string(port));
    require(clientConn != nullptr, "client should connect for batch test");

    // Send three datagrams in sequence.
    const std::vector<std::string> payloads = {"batch-0", "batch-1", "batch-2"};
    for (const auto& p : payloads) {
        rsp::Buffer b(reinterpret_cast<const uint8_t*>(p.data()),
                      static_cast<uint32_t>(p.size()));
        clientConn->send(b);
    }

    require(serverConnFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "server should receive first datagram of batch");

    rsp::transport::ConnectionHandle serverConn = serverConnFuture.get();
    require(serverConn != nullptr, "server-side connection should be non-null for batch");

    for (const auto& expected : payloads) {
        rsp::Buffer b;
        const int n = serverConn->recv(b);
        require(n == static_cast<int>(expected.size()),
                "server should receive correct datagram size in batch");
        require(std::memcmp(b.data(), expected.data(), expected.size()) == 0,
                "server should receive correct datagram payload in batch");
    }

    clientTransport->stop();
    serverTransport->stop();
}

}  // namespace

int main() {
    try {
        std::cout << "testUdpClientServerRoundTrip" << std::endl;
        testUdpClientServerRoundTrip();

        std::cout << "testUdpMultiplePeersDemux" << std::endl;
        testUdpMultiplePeersDemux();

        std::cout << "testUdpMultipleDatgramBatch" << std::endl;
        testUdpMultipleDatgramBatch();

        std::cout << "all UDP transport tests passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test failed: " << e.what() << std::endl;
        return 1;
    }
}
