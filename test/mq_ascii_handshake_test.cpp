#include "common/message_queue/mq_ascii_handshake.hpp"

#include "common/transport/transport_memory.hpp"
#include "common/version.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* kMessageTerminator = "\r\n\r\n";

rsp::Buffer stringToBuffer(const std::string& value) {
    if (value.empty()) {
        return rsp::Buffer();
    }

    return rsp::Buffer(reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size()));
}

bool sendAll(const rsp::transport::ConnectionHandle& connection, const std::string& message) {
    if (connection == nullptr) {
        return false;
    }

    const rsp::Buffer buffer = stringToBuffer(message);
    return connection->sendAll(buffer.data(), buffer.size());
}

bool receiveMessage(const rsp::transport::ConnectionHandle& connection, std::string& message) {
    if (connection == nullptr) {
        return false;
    }

    message.clear();
    rsp::Buffer buffer(256);
    while (message.find(kMessageTerminator) == std::string::npos) {
        const int bytesRead = connection->recv(buffer);
        if (bytesRead <= 0) {
            return false;
        }

        message.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(bytesRead));
        if (message.size() > 4096) {
            return false;
        }
    }

    return true;
}

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

std::string expectedServerAdvertisement() {
    return std::string("RSP version ") + RSP_VERSION + "\r\n"
           "encodings:protobuf\r\n"
           "asymmetric: P256\r\n"
           "\r\n";
}

void testServerQueueSuccess() {
    auto receivedMessages = std::make_shared<rsp::BufferedMessageQueue>();
    std::optional<rsp::encoding::EncodingHandle> createdEncoding;
    std::atomic<bool> failureCalled = false;

    rsp::message_queue::MessageQueueAsciiHandshakeServer queue(
        receivedMessages,
        rsp::KeyPair::generateP256(),
        [&createdEncoding](const rsp::encoding::EncodingHandle& newEncoding) { createdEncoding = newEncoding; },
        [&failureCalled](const rsp::transport::ConnectionHandle&) { failureCalled = true; });
    queue.setWorkerCount(1);
    queue.start();

    auto listener = std::make_shared<rsp::transport::MemoryTransport>();
    listener->setNewConnectionCallback([&queue](const rsp::transport::ConnectionHandle& connection) {
        queue.push(connection);
    });
    require(listener->listen("mq-ascii-server-success"), "listener should accept server test connections");

    auto clientTransport = std::make_shared<rsp::transport::MemoryTransport>();
    const auto clientConnection = clientTransport->connect("mq-ascii-server-success");
    require(clientConnection != nullptr, "client should connect to server queue listener");

    std::string serverAdvertisement;
    require(receiveMessage(clientConnection, serverAdvertisement), "client should receive server advertisement");
    require(serverAdvertisement == expectedServerAdvertisement(), "server advertisement should match the protocol");
    require(sendAll(clientConnection, "encoding:protobuf\r\n\r\n"), "client should send supported encoding");

    std::string response;
    require(receiveMessage(clientConnection, response), "client should receive handshake success response");
    require(response == "1success: encoding:protobuf\r\n\r\n", "server should confirm protobuf encoding");
    require(waitForCondition([&createdEncoding]() { return createdEncoding.has_value(); }),
            "server queue should create an encoding after handshake success");
    require(createdEncoding.value() != nullptr, "server queue should report a non-null encoding");
    require(!failureCalled.load(), "server queue should not report failure on successful handshake");

    queue.stop();
    listener->stop();
    clientTransport->stop();
}

void testServerQueueUnsupportedEncoding() {
    auto receivedMessages = std::make_shared<rsp::BufferedMessageQueue>();
    std::atomic<bool> successCalled = false;
    std::atomic<bool> failureCalled = false;

    rsp::message_queue::MessageQueueAsciiHandshakeServer queue(
        receivedMessages,
        rsp::KeyPair::generateP256(),
        [&successCalled](const rsp::encoding::EncodingHandle&) { successCalled = true; },
        [&failureCalled](const rsp::transport::ConnectionHandle&) { failureCalled = true; });
    queue.setWorkerCount(1);
    queue.start();

    auto listener = std::make_shared<rsp::transport::MemoryTransport>();
    listener->setNewConnectionCallback([&queue](const rsp::transport::ConnectionHandle& connection) {
        queue.push(connection);
    });
    require(listener->listen("mq-ascii-server-failure"), "listener should accept failure test connections");

    auto clientTransport = std::make_shared<rsp::transport::MemoryTransport>();
    const auto clientConnection = clientTransport->connect("mq-ascii-server-failure");
    require(clientConnection != nullptr, "client should connect to failure listener");

    std::string serverAdvertisement;
    require(receiveMessage(clientConnection, serverAdvertisement), "client should receive server advertisement before failure");
    require(sendAll(clientConnection, "encoding:json\r\n\r\n"), "client should send unsupported encoding");

    std::string response;
    require(receiveMessage(clientConnection, response), "client should receive handshake error response");
    require(response == "0error: unsupported encoding\r\n\r\n", "server should reject unsupported encodings");
    require(waitForCondition([&failureCalled]() { return failureCalled.load(); }),
            "server queue should report failure on unsupported encoding");
    require(!successCalled.load(), "server queue should not report success on unsupported encoding");

    queue.stop();
    listener->stop();
    clientTransport->stop();
}

void testClientQueueSuccess() {
    auto receivedMessages = std::make_shared<rsp::BufferedMessageQueue>();
    std::optional<rsp::encoding::EncodingHandle> createdEncoding;
    std::atomic<bool> failureCalled = false;

    rsp::message_queue::MessageQueueAsciiHandshakeClient queue(
        receivedMessages,
        rsp::KeyPair::generateP256(),
        rsp::message_queue::kAsciiHandshakeEncoding,
        [&createdEncoding](const rsp::encoding::EncodingHandle& newEncoding) { createdEncoding = newEncoding; },
        [&failureCalled](const rsp::transport::TransportHandle&) { failureCalled = true; });
    queue.setWorkerCount(1);
    queue.start();

    auto listener = std::make_shared<rsp::transport::MemoryTransport>();
    std::promise<void> serverDone;
    auto serverDoneFuture = serverDone.get_future();
    listener->setNewConnectionCallback([&serverDone](const rsp::transport::ConnectionHandle& connection) {
        std::thread([connection, &serverDone]() mutable {
            try {
                require(sendAll(connection, expectedServerAdvertisement()), "manual server should send advertisement");
                std::string selection;
                require(receiveMessage(connection, selection), "manual server should receive client selection");
                require(selection == "encoding:protobuf\r\n\r\n", "client queue should request protobuf encoding");
                require(sendAll(connection, "1success: encoding:protobuf\r\n\r\n"),
                        "manual server should send success response");
                serverDone.set_value();
            } catch (...) {
                serverDone.set_exception(std::current_exception());
            }
        }).detach();
    });
    require(listener->listen("mq-ascii-client-success"), "listener should accept client queue test connections");

    auto clientTransport = std::make_shared<rsp::transport::MemoryTransport>();
    require(clientTransport->connect("mq-ascii-client-success") != nullptr,
            "client transport should connect before queue processing");
    require(queue.push(clientTransport), "client queue should accept the transport");

    require(serverDoneFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "manual server should complete the handshake exchange");
    serverDoneFuture.get();
    require(waitForCondition([&createdEncoding]() { return createdEncoding.has_value(); }),
            "client queue should create an encoding after handshake success");
    require(createdEncoding.value() != nullptr, "client queue should report a non-null encoding");
    require(!failureCalled.load(), "client queue should not report failure on successful handshake");

    queue.stop();
    listener->stop();
    clientTransport->stop();
}

void testClientQueueFailure() {
    auto receivedMessages = std::make_shared<rsp::BufferedMessageQueue>();
    std::atomic<bool> successCalled = false;
    std::atomic<bool> failureCalled = false;

    rsp::message_queue::MessageQueueAsciiHandshakeClient queue(
        receivedMessages,
        rsp::KeyPair::generateP256(),
        rsp::message_queue::kAsciiHandshakeEncoding,
        [&successCalled](const rsp::encoding::EncodingHandle&) { successCalled = true; },
        [&failureCalled](const rsp::transport::TransportHandle&) { failureCalled = true; });
    queue.setWorkerCount(1);
    queue.start();

    auto listener = std::make_shared<rsp::transport::MemoryTransport>();
    std::promise<void> serverDone;
    auto serverDoneFuture = serverDone.get_future();
    listener->setNewConnectionCallback([&serverDone](const rsp::transport::ConnectionHandle& connection) {
        std::thread([connection, &serverDone]() mutable {
            try {
                require(sendAll(connection, expectedServerAdvertisement()), "manual server should send advertisement");
                std::string selection;
                require(receiveMessage(connection, selection), "manual server should receive client selection");
                require(sendAll(connection, "0error: unsupported encoding\r\n\r\n"),
                        "manual server should send failure response");
                serverDone.set_value();
            } catch (...) {
                serverDone.set_exception(std::current_exception());
            }
        }).detach();
    });
    require(listener->listen("mq-ascii-client-failure"), "listener should accept client failure test connections");

    auto clientTransport = std::make_shared<rsp::transport::MemoryTransport>();
    require(clientTransport->connect("mq-ascii-client-failure") != nullptr,
            "client transport should connect before queue failure processing");
    require(queue.push(clientTransport), "client queue should accept the transport before failure");

    require(serverDoneFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "manual server should complete the failure exchange");
    serverDoneFuture.get();
    require(waitForCondition([&failureCalled]() { return failureCalled.load(); }),
            "client queue should report failure on server rejection");
    require(!successCalled.load(), "client queue should not report success when the server rejects the encoding");

    queue.stop();
    listener->stop();
    clientTransport->stop();
}

}  // namespace

int main() {
    try {
        testServerQueueSuccess();
        testServerQueueUnsupportedEncoding();
        testClientQueueSuccess();
        testClientQueueFailure();

        std::cout << "mq_ascii_handshake_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mq_ascii_handshake_test failed: " << exception.what() << '\n';
        return 1;
    }
}