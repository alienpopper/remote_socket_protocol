#include "client/cpp/rsp_client.hpp"
#include "client/cpp/rsp_client_message.hpp"

#include "messages.pb.h"
#include "common/transport/transport_tcp.hpp"
#include "resource_manager/resource_manager.hpp"

#include "common/transport/transport.hpp"

#include <array>
#include <cstring>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class TestResourceManager : public rsp::resource_manager::ResourceManager {
public:
    using rsp::resource_manager::ResourceManager::ResourceManager;

    rsp::NodeID nodeId() const {
        return keyPair().nodeID();
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void setBytes(std::string* destination, const std::array<uint8_t, 16>& value) {
    destination->assign(reinterpret_cast<const char*>(value.data()), value.size());
}

void setNodeIdBytes(std::string* destination, const rsp::NodeID& nodeId) {
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    destination->assign(16, '\0');
    std::memcpy(destination->data(), &high, sizeof(high));
    std::memcpy(destination->data() + sizeof(high), &low, sizeof(low));
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

rsp::proto::RSPMessage makeRouteUpdateMessage(const std::array<uint8_t, 16>& nodeIdBytes, uint32_t hopsAway) {
    rsp::proto::RSPMessage message;
    auto* entry = message.mutable_route()->add_entries();
    setBytes(entry->mutable_node_id()->mutable_value(), nodeIdBytes);
    entry->set_hops_away(hopsAway);
    return message;
}

rsp::proto::RSPMessage makePingRequestMessage(const rsp::NodeID& sourceNodeId,
                                              const rsp::NodeID& destinationNodeId,
                                              const std::string& nonce,
                                              uint32_t sequence,
                                              uint64_t timeSentMilliseconds) {
    rsp::proto::RSPMessage message;
    setNodeIdBytes(message.mutable_source()->mutable_value(), sourceNodeId);
    setNodeIdBytes(message.mutable_destination()->mutable_value(), destinationNodeId);
    message.mutable_ping_request()->mutable_nonce()->set_value(nonce);
    message.mutable_ping_request()->set_sequence(sequence);
    message.mutable_ping_request()->mutable_time_sent()->set_milliseconds_since_epoch(timeSentMilliseconds);
    return message;
}

std::string findListeningEndpoint(const std::shared_ptr<rsp::transport::TcpTransport>& serverTransport) {
    for (uint16_t port = 35000; port < 35100; ++port) {
        const std::string endpoint = std::string("127.0.0.1:") + std::to_string(port);
        if (serverTransport->listen(endpoint)) {
            return endpoint;
        }
    }

    throw std::runtime_error("failed to find an available TCP port for handshake test");
}

void testTcpAsciiHandshake() {
    auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair clientKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID clientNodeId = clientKeyPair.nodeID();

    std::promise<rsp::encoding::EncodingHandle> handshakePromise;
    std::future<rsp::encoding::EncodingHandle> handshakeFuture = handshakePromise.get_future();
    resourceManager.setNewEncodingCallback([&handshakePromise](const rsp::encoding::EncodingHandle& encoding) {
        try {
            handshakePromise.set_value(encoding);
        } catch (...) {
            handshakePromise.set_exception(std::current_exception());
        }
    });

    const std::string endpoint = findListeningEndpoint(serverTransport);
    const std::string transportSpec = std::string("tcp:") + endpoint;

    rsp::client::RSPClientMessage::Ptr client = rsp::client::RSPClientMessage::create(std::move(clientKeyPair));
    const rsp::client::RSPClientMessage::ClientConnectionID connectionId =
        client->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
    require(client->hasConnections(), "client should track created connections");
    require(client->hasConnection(connectionId), "client should track the new connection id");
    require(client->connectionCount() == 1, "client should track one live connection");
    require(client->connectionIds().size() == 1, "client should enumerate live connection ids");

    require(handshakeFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "server handshake pipeline should complete");
    const rsp::encoding::EncodingHandle serverEncoding = handshakeFuture.get();
    require(serverEncoding != nullptr, "server should activate an authenticated encoding");
    require(resourceManager.activeEncodingCount() == 1,
            "resource manager should create an encoding for the accepted connection");
    require(resourceManager.pendingMessageCount() == 0,
        "authentication messages should not be exposed through the resource manager queue");

    const auto clientPeerNodeId = client->peerNodeID(connectionId);
    require(clientPeerNodeId.has_value(), "client encoding should learn the server node id during authentication");
    require(clientPeerNodeId.value() == resourceManager.nodeId(),
        "client encoding should store the resource manager node id");

    const auto serverPeerNodeId = serverEncoding->peerNodeID();
    require(serverPeerNodeId.has_value(), "server encoding should learn the client node id during authentication");
    require(serverPeerNodeId.value() == clientNodeId,
        "server encoding should store the client node id");

    const rsp::proto::RSPMessage pingRequest =
        makePingRequestMessage(clientNodeId, resourceManager.nodeId(), "ping-nonce", 7, 123456);
    require(client->send(pingRequest), "client should send a ping request after authentication");
    require(waitForCondition([&client]() { return client->pendingMessageCount() == 1; }),
        "client should receive a ping reply from the resource manager");

    rsp::proto::RSPMessage pingReply;
    require(client->tryDequeueMessage(pingReply), "client should expose the ping reply decoded by its encoding");
    require(pingReply.has_ping_reply(), "resource manager should answer ping requests with ping replies");
    require(pingReply.destination().value() == pingRequest.source().value(),
        "ping reply should target the client node id");
    require(pingReply.source().value() == pingRequest.destination().value(),
        "ping reply should identify the resource manager as sender");
    require(pingReply.ping_reply().nonce().value() == pingRequest.ping_request().nonce().value(),
        "ping reply should preserve the ping nonce");
    require(pingReply.ping_reply().sequence() == pingRequest.ping_request().sequence(),
        "ping reply should preserve the ping sequence");
    require(pingReply.ping_reply().time_sent().milliseconds_since_epoch() ==
                pingRequest.ping_request().time_sent().milliseconds_since_epoch(),
        "ping reply should preserve the original send timestamp");
    require(pingReply.ping_reply().has_time_replied(),
        "ping reply should include the reply timestamp");

    const std::array<uint8_t, 16> serverRouteNode = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                         0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
    const rsp::proto::RSPMessage serverMessage = makeRouteUpdateMessage(serverRouteNode, 5);
    require(resourceManager.sendToConnection(0, serverMessage),
        "resource manager should send framed protobuf messages through its active encoding");
    require(waitForCondition([&client]() { return client->pendingMessageCount() == 1; }),
        "client should receive and queue a decoded protobuf message");

    rsp::proto::RSPMessage queuedAtClient;
    require(client->tryDequeueMessage(queuedAtClient), "client should expose queued decoded protobuf messages");
    require(queuedAtClient.has_route(), "client should decode the server route update");
    require(queuedAtClient.route().entries_size() == 1, "client should preserve route entries");
    require(queuedAtClient.route().entries(0).node_id().value() == serverMessage.route().entries(0).node_id().value(),
        "client should preserve the route payload across framing");

    require(client->removeConnection(connectionId), "client should remove an existing connection");
    require(!client->hasConnection(connectionId), "removed connection should no longer be tracked");
    require(client->connectionCount() == 0, "removing a connection should shrink the managed set");
    require(!client->removeConnection(connectionId), "removing the same connection twice should fail");
    serverTransport->stop();
}

void testClientToClientRouting() {
    auto serverTransport = std::make_shared<rsp::transport::TcpTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair firstClientKeyPair = rsp::KeyPair::generateP256();
    rsp::KeyPair secondClientKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID secondClientNodeId = secondClientKeyPair.nodeID();

    const std::string endpoint = findListeningEndpoint(serverTransport);
    const std::string transportSpec = std::string("tcp:") + endpoint;

    rsp::client::RSPClient::Ptr firstClient = rsp::client::RSPClient::create(std::move(firstClientKeyPair));
    rsp::client::RSPClient::Ptr secondClient = rsp::client::RSPClient::create(std::move(secondClientKeyPair));

    const rsp::client::RSPClient::ClientConnectionID firstConnectionId =
        firstClient->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);
    const rsp::client::RSPClient::ClientConnectionID secondConnectionId =
        secondClient->connectToResourceManager(transportSpec, rsp::ascii_handshake::kEncoding);

    require(firstClient->hasConnections(), "high-level client should track created connections");
    require(firstClient->hasConnection(firstConnectionId), "high-level client should expose existing connection ids");
    require(firstClient->connectionCount() == 1, "high-level client should report one live connection");
    require(firstClient->connectionIds().size() == 1, "high-level client should enumerate its connections");
    require(secondClient->hasConnection(secondConnectionId), "second high-level client should expose its connection id");

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
        "resource manager should authenticate both client connections");

    require(firstClient->ping(secondClientNodeId), "high-level client should route a ping to another client through the resource manager");

    require(firstClient->removeConnection(firstConnectionId), "high-level client should remove an existing connection");
    require(secondClient->removeConnection(secondConnectionId), "second high-level client should remove an existing connection");
}

}  // namespace

int main() {
    try {
        rsp::client::RSPClientMessage::Ptr client = rsp::client::RSPClientMessage::create();
        require(client != nullptr, "client should be reference counted");
        require(!client->hasConnections(), "client should start without connections");
        require(client->connectionCount() == 0, "client should start with zero connections");

        rsp::client::RSPClientMessage::Ptr secondReference = client;
        require(secondReference.use_count() >= 2, "client should support shared ownership");

        bool invalidTransportThrown = false;
        try {
            static_cast<void>(client->connectToResourceManager("invalid-transport-spec", "protobuf"));
        } catch (const std::invalid_argument&) {
            invalidTransportThrown = true;
        }
        require(invalidTransportThrown, "client should reject malformed transport specifications");

        bool unsupportedTransportThrown = false;
        try {
            static_cast<void>(client->connectToResourceManager("udp:127.0.0.1:5555", "protobuf"));
        } catch (const std::invalid_argument&) {
            unsupportedTransportThrown = true;
        }
        require(unsupportedTransportThrown, "client should reject unsupported transport names");

        testTcpAsciiHandshake();
        testClientToClientRouting();

        std::cout << "client_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "client_test failed: " << exception.what() << '\n';
        return 1;
    }
}