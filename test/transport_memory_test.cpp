#include "client/cpp/rsp_client_message.hpp"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/transport/transport_memory.hpp"
#include "messages.pb.h"
#include "resource_manager/resource_manager.hpp"
#include "resource_service/bsd_sockets/resource_service_bsd_sockets.hpp"

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

void setNodeIdBytes(std::string* destination, const rsp::NodeID& nodeId) {
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    destination->assign(16, '\0');
    std::memcpy(destination->data(), &high, sizeof(high));
    std::memcpy(destination->data() + sizeof(high), &low, sizeof(low));
}

rsp::proto::RSPMessage makePingRequest(const rsp::NodeID& sourceNodeId,
                                       const rsp::NodeID& destinationNodeId,
                                       const std::string& nonce,
                                       uint32_t sequence) {
    rsp::proto::RSPMessage message;
    setNodeIdBytes(message.mutable_source()->mutable_value(), sourceNodeId);
    setNodeIdBytes(message.mutable_destination()->mutable_value(), destinationNodeId);
    message.mutable_ping_request()->mutable_nonce()->set_value(nonce);
    message.mutable_ping_request()->set_sequence(sequence);
    message.mutable_ping_request()->mutable_time_sent()->set_milliseconds_since_epoch(0);
    return message;
}

// Verifies that a client can connect to RM via memory transport, perform the full
// handshake and authentication pipeline, send a ping, and receive a reply.
void testMemoryTransportHandshake() {
    auto rmTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({rmTransport});

    std::promise<rsp::encoding::EncodingHandle> handshakePromise;
    std::future<rsp::encoding::EncodingHandle> handshakeFuture = handshakePromise.get_future();
    resourceManager.setNewEncodingCallback([&handshakePromise](const rsp::encoding::EncodingHandle& encoding) {
        try {
            handshakePromise.set_value(encoding);
        } catch (...) {
            handshakePromise.set_exception(std::current_exception());
        }
    });

    require(rmTransport->listen("mem-handshake-channel"),
            "RM memory transport should begin listening on the named channel");

    rsp::client::RSPClientMessage::Ptr client = rsp::client::RSPClientMessage::create();
    const auto connectionId =
        client->connectToResourceManager("memory:mem-handshake-channel", rsp::message_queue::kAsciiHandshakeEncoding).value();

    require(client->hasConnections(), "client should track the memory transport connection");
    require(client->hasConnection(connectionId), "client should expose the new connection id");
    require(client->connectionCount() == 1, "client should report exactly one live connection");

    require(handshakeFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "RM should complete handshake for memory transport connection");
    const rsp::encoding::EncodingHandle serverEncoding = handshakeFuture.get();
    require(serverEncoding != nullptr, "RM should produce an authenticated encoding over memory transport");
    require(resourceManager.activeEncodingCount() == 1,
            "RM should have exactly one active encoding after memory transport handshake");

    const auto clientPeerNodeId = client->peerNodeID(connectionId);
    require(clientPeerNodeId.has_value(),
            "client should learn the RM node id during memory transport authentication");
    require(clientPeerNodeId.value() == resourceManager.nodeId(),
            "client peer node id should match the RM node id");

    const rsp::proto::RSPMessage pingRequest =
        makePingRequest(client->nodeId(), resourceManager.nodeId(), "mem-nonce", 42);
    require(client->send(pingRequest), "client should send a ping over memory transport");
    require(waitForCondition([&client]() { return client->pendingMessageCount() == 1; }),
            "client should receive a ping reply over memory transport");

    rsp::proto::RSPMessage pingReply;
    require(client->tryDequeueMessage(pingReply), "client should dequeue the ping reply");
    require(pingReply.has_ping_reply(), "RM should reply to ping requests over memory transport");
    require(pingReply.ping_reply().nonce().value() == pingRequest.ping_request().nonce().value(),
            "ping reply should preserve the nonce");
    require(pingReply.ping_reply().sequence() == pingRequest.ping_request().sequence(),
            "ping reply should preserve the sequence number");
    require(pingReply.ping_reply().has_time_replied(),
            "ping reply should include the reply timestamp");

    client->removeConnection(connectionId);
    rmTransport->stop();
}

// Verifies the RS->RM->Client in-process scenario using memory transport exclusively.
// RS and a simple client both connect to RM via memory socket pairs; no TCP is involved.
void testMemoryTransportResourceServicePing() {
    auto rmTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({rmTransport});

    require(rmTransport->listen("mem-rs-rm-channel"),
            "RM memory transport should listen for in-process connections");

    // Connect RS to RM via memory transport.
    auto resourceService = rsp::resource_service::BsdSocketsResourceService::create();
    const auto rsConnectionId =
        resourceService->connectToResourceManager("memory:mem-rs-rm-channel", rsp::message_queue::kAsciiHandshakeEncoding);
    require(resourceService->hasConnections(), "RS should connect to RM via memory transport");
    require(resourceService->hasConnection(rsConnectionId), "RS should track its connection to RM");

    // Wait for RS to complete authentication and send its resource advertisement to RM.
    require(waitForCondition([&resourceManager, &resourceService]() {
                return resourceManager.hasResourceAdvertisement(resourceService->nodeId());
            }),
            "RM should receive RS resource advertisement over memory transport");

    // Connect a simple client to RM via memory transport.
    rsp::client::RSPClientMessage::Ptr client = rsp::client::RSPClientMessage::create();
    const auto clientConnectionId =
        client->connectToResourceManager("memory:mem-rs-rm-channel", rsp::message_queue::kAsciiHandshakeEncoding).value();
    require(client->hasConnections(), "client should connect to RM via memory transport");

    // Wait for RM to authenticate both connections.
    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
            "RM should have two authenticated connections (RS and client) over memory transport");

    // Send a ping from the client to RM. The message travels entirely in-process.
    const rsp::proto::RSPMessage pingRequest =
        makePingRequest(client->nodeId(), resourceManager.nodeId(), "rs-rm-client-nonce", 1);
    require(client->send(pingRequest), "client should send ping to RM via memory transport");
    require(waitForCondition([&client]() { return client->pendingMessageCount() == 1; }),
            "client should receive ping reply from RM over memory transport");

    rsp::proto::RSPMessage pingReply;
    require(client->tryDequeueMessage(pingReply), "client should dequeue the ping reply");
    require(pingReply.has_ping_reply(), "RM should reply to ping over in-process memory transport");
    require(pingReply.ping_reply().nonce().value() == pingRequest.ping_request().nonce().value(),
            "ping reply nonce should match the request");
    require(pingReply.ping_reply().has_time_replied(),
            "ping reply should include a reply timestamp");

    // Confirm RS advertisement is still present (connection is live).
    require(resourceManager.hasResourceAdvertisement(resourceService->nodeId()),
            "RS resource advertisement should remain registered while RS is connected via memory transport");

    client->removeConnection(clientConnectionId);
    resourceService->removeConnection(rsConnectionId);
    rmTransport->stop();
}

}  // namespace

int main() {
    try {
        std::cout << "testMemoryTransportHandshake" << std::endl;
        testMemoryTransportHandshake();

        std::cout << "testMemoryTransportResourceServicePing" << std::endl;
        testMemoryTransportResourceServicePing();

        std::cout << "all memory transport tests passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test failed: " << e.what() << std::endl;
        return 1;
    }
}
