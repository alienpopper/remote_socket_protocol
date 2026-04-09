#include "common/encoding/protobuf/protobuf_encoding.hpp"
#include "common/message_queue/mq_authn.hpp"
#include "common/transport/transport_memory.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr uint32_t kFrameMagic = 0x52535050U;
constexpr uint32_t kFrameHeaderSize = 8;

void appendUint32(std::string& buffer, uint32_t value) {
    buffer.push_back(static_cast<char>((value >> 24) & 0xFFU));
    buffer.push_back(static_cast<char>((value >> 16) & 0xFFU));
    buffer.push_back(static_cast<char>((value >> 8) & 0xFFU));
    buffer.push_back(static_cast<char>(value & 0xFFU));
}

uint32_t readUint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

std::string randomNonceBytes() {
    return std::string(16, 'n');
}

bool readFramedMessage(const rsp::transport::ConnectionHandle& connection, rsp::proto::RSPMessage& message) {
    uint8_t header[kFrameHeaderSize] = {};
    if (!connection->readExact(header, kFrameHeaderSize)) {
        return false;
    }

    if (readUint32(header) != kFrameMagic) {
        return false;
    }

    const uint32_t payloadLength = readUint32(header + 4);
    std::string payload(payloadLength, '\0');
    if (!connection->readExact(reinterpret_cast<uint8_t*>(payload.data()), payloadLength)) {
        return false;
    }

    return message.ParseFromArray(payload.data(), static_cast<int>(payload.size()));
}

bool writeFramedMessage(const rsp::transport::ConnectionHandle& connection, const rsp::proto::RSPMessage& message) {
    std::string payload;
    if (!message.SerializeToString(&payload)) {
        return false;
    }

    std::string header;
    header.reserve(kFrameHeaderSize);
    appendUint32(header, kFrameMagic);
    appendUint32(header, static_cast<uint32_t>(payload.size()));

    return connection->sendAll(reinterpret_cast<const uint8_t*>(header.data()), static_cast<uint32_t>(header.size())) &&
           connection->sendAll(reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()));
}

rsp::Buffer serializeUnsignedMessage(const rsp::proto::RSPMessage& message) {
    rsp::proto::RSPMessage unsignedMessage = message;
    unsignedMessage.clear_signature();

    std::string payload;
    if (!unsignedMessage.SerializeToString(&payload)) {
        throw std::runtime_error("failed to serialize unsigned authentication message");
    }

    if (payload.empty()) {
        return rsp::Buffer();
    }

    return rsp::Buffer(reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()));
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

void testAuthNSuccess() {
    auto listener = std::make_shared<rsp::transport::MemoryTransport>();
    std::promise<rsp::transport::ConnectionHandle> serverConnectionPromise;
    auto serverConnectionFuture = serverConnectionPromise.get_future();
    listener->setNewConnectionCallback([&serverConnectionPromise](const rsp::transport::ConnectionHandle& connection) {
        serverConnectionPromise.set_value(connection);
    });
    require(listener->listen("mq-authn-success"), "listener should accept authn test connections");

    auto clientTransport = std::make_shared<rsp::transport::MemoryTransport>();
    const auto clientConnection = clientTransport->connect("mq-authn-success");
    require(clientConnection != nullptr, "client should connect to authn listener");
    require(serverConnectionFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "server side should observe the authn connection");

    auto receivedMessages = std::make_shared<rsp::BufferedMessageQueue>();
    auto localKeyPair = rsp::KeyPair::generateP256();
    auto encoding = std::make_shared<rsp::encoding::protobuf::ProtobufEncoding>(clientConnection,
                                                                                receivedMessages,
                                                                                localKeyPair.duplicate());

    std::atomic<bool> successCalled = false;
    std::atomic<bool> failureCalled = false;
    std::optional<rsp::proto::Identity> storedIdentity;

    rsp::message_queue::MessageQueueAuthN queue(
        localKeyPair.duplicate(),
        [&successCalled](const rsp::encoding::EncodingHandle&) { successCalled = true; },
        [&failureCalled](const rsp::encoding::EncodingHandle&) { failureCalled = true; },
        [&storedIdentity](const rsp::NodeID&, const rsp::proto::Identity& identity) { storedIdentity = identity; });
    queue.setWorkerCount(1);
    queue.start();

    auto serverConnection = serverConnectionFuture.get();
    auto peerKeyPair = rsp::KeyPair::generateP256();

    auto peerFuture = std::async(std::launch::async, [serverConnection, peerKeyPair = std::move(peerKeyPair)]() mutable {
        rsp::proto::RSPMessage initialChallenge;
        require(readFramedMessage(serverConnection, initialChallenge), "peer should receive authn challenge");
        require(initialChallenge.has_challenge_request(), "first authn frame should be a challenge request");

        rsp::proto::RSPMessage peerChallenge;
        peerChallenge.mutable_challenge_request()->mutable_nonce()->set_value(randomNonceBytes());
        require(writeFramedMessage(serverConnection, peerChallenge), "peer should send its challenge request");

        rsp::proto::RSPMessage localIdentity;
        require(readFramedMessage(serverConnection, localIdentity), "peer should receive local identity");
        require(localIdentity.has_identity(), "peer should receive local identity message");
        require(localIdentity.identity().nonce().value() == peerChallenge.challenge_request().nonce().value(),
                "local identity should answer the peer challenge nonce");

        rsp::proto::RSPMessage peerIdentity;
        peerIdentity.mutable_identity()->mutable_nonce()->CopyFrom(initialChallenge.challenge_request().nonce());
        *peerIdentity.mutable_identity()->mutable_public_key() = peerKeyPair.publicKey();
        *peerIdentity.mutable_signature() = peerKeyPair.signBlock(serializeUnsignedMessage(peerIdentity));
        require(writeFramedMessage(serverConnection, peerIdentity), "peer should send signed identity response");
    });

    require(queue.push(encoding), "authn queue should accept the encoding");
    require(peerFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "peer flow should finish the identity exchange");
    peerFuture.get();
    require(waitForCondition([&successCalled]() { return successCalled.load(); }),
            "authn queue should report success after a valid exchange");
    require(!failureCalled.load(), "authn queue should not report failure on a valid exchange");
    require(storedIdentity.has_value(), "authn queue should expose the received peer identity through storeIdentity");
    require(encoding->peerNodeID().has_value(), "authn queue should establish the peer node id on the encoding");

    queue.stop();
    listener->stop();
    clientTransport->stop();
}

void testAuthNFailureOnInvalidIdentity() {
    auto listener = std::make_shared<rsp::transport::MemoryTransport>();
    std::promise<rsp::transport::ConnectionHandle> serverConnectionPromise;
    auto serverConnectionFuture = serverConnectionPromise.get_future();
    listener->setNewConnectionCallback([&serverConnectionPromise](const rsp::transport::ConnectionHandle& connection) {
        serverConnectionPromise.set_value(connection);
    });
    require(listener->listen("mq-authn-failure"), "listener should accept invalid authn test connections");

    auto clientTransport = std::make_shared<rsp::transport::MemoryTransport>();
    const auto clientConnection = clientTransport->connect("mq-authn-failure");
    require(clientConnection != nullptr, "client should connect to invalid authn listener");
    require(serverConnectionFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "server side should observe the invalid authn connection");

    auto receivedMessages = std::make_shared<rsp::BufferedMessageQueue>();
    auto localKeyPair = rsp::KeyPair::generateP256();
    auto encoding = std::make_shared<rsp::encoding::protobuf::ProtobufEncoding>(clientConnection,
                                                                                receivedMessages,
                                                                                localKeyPair.duplicate());

    std::atomic<bool> successCalled = false;
    std::atomic<bool> failureCalled = false;
    std::atomic<bool> storeIdentityCalled = false;

    rsp::message_queue::MessageQueueAuthN queue(
        localKeyPair.duplicate(),
        [&successCalled](const rsp::encoding::EncodingHandle&) { successCalled = true; },
        [&failureCalled](const rsp::encoding::EncodingHandle&) { failureCalled = true; },
        [&storeIdentityCalled](const rsp::NodeID&, const rsp::proto::Identity&) { storeIdentityCalled = true; });
    queue.setWorkerCount(1);
    queue.start();

    auto serverConnection = serverConnectionFuture.get();
    auto peerKeyPair = rsp::KeyPair::generateP256();

    auto peerFuture = std::async(std::launch::async, [serverConnection, peerKeyPair = std::move(peerKeyPair)]() mutable {
        rsp::proto::RSPMessage initialChallenge;
        require(readFramedMessage(serverConnection, initialChallenge), "peer should receive authn challenge");
        require(initialChallenge.has_challenge_request(), "first authn frame should be a challenge request");

        rsp::proto::RSPMessage invalidIdentity;
        invalidIdentity.mutable_identity()->mutable_nonce()->set_value("wrong-wrong-wrong!");
        *invalidIdentity.mutable_identity()->mutable_public_key() = peerKeyPair.publicKey();
        *invalidIdentity.mutable_signature() = peerKeyPair.signBlock(serializeUnsignedMessage(invalidIdentity));
        require(writeFramedMessage(serverConnection, invalidIdentity), "peer should send invalid identity response");
    });

    require(queue.push(encoding), "authn queue should accept the encoding before invalid authn");
    require(peerFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "peer should finish sending the invalid identity");
    peerFuture.get();
    require(waitForCondition([&failureCalled]() { return failureCalled.load(); }),
            "authn queue should report failure on invalid identity");
    require(!successCalled.load(), "authn queue should not report success on invalid identity");
    require(!storeIdentityCalled.load(), "authn queue should not store invalid identities");
    require(!encoding->peerNodeID().has_value(), "authn queue should not establish a peer id after invalid identity");

    queue.stop();
    listener->stop();
    clientTransport->stop();
}

}  // namespace

int main() {
    try {
        testAuthNSuccess();
        testAuthNFailureOnInvalidIdentity();

        std::cout << "mq_authn_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mq_authn_test failed: " << exception.what() << '\n';
        return 1;
    }
}