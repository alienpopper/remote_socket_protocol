#include "client/cpp/rsp_client.hpp"
#include "client/cpp/rsp_client_message.hpp"
#include "client/cpp_full/rsp_client.hpp"

#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "common/ping_trace.hpp"
#include "messages.pb.h"
#include "common/transport/transport_memory.hpp"
#include "common/transport/transport_tcp.hpp"
#include "resource_manager/resource_manager.hpp"

#include "common/transport/transport.hpp"

#include <array>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct PingStats {
    double minimumMilliseconds = 0.0;
    double averageMilliseconds = 0.0;
    double maximumMilliseconds = 0.0;
};

struct PingBreakdown {
    std::vector<std::pair<std::string, double>> segments;
    double serializationMilliseconds = 0.0;
    double threadWakeupMilliseconds = 0.0;
    double transmissionMilliseconds = 0.0;
    double handlerMilliseconds = 0.0;
    double totalMilliseconds = 0.0;
};

struct ClientToClientPingResults {
    PingStats stats;
    PingBreakdown breakdown;
};

class TestFullClient : public rsp::client::full::RSPClient {
public:
    using Ptr = std::shared_ptr<TestFullClient>;

    static Ptr create(rsp::KeyPair keyPair) {
        return Ptr(new TestFullClient(std::move(keyPair)));
    }

    bool tryDequeueHandledMessage(rsp::proto::RSPMessage& message) {
        std::lock_guard<std::mutex> lock(handledMessagesMutex_);
        if (handledMessages_.empty()) {
            return false;
        }

        message = std::move(handledMessages_.front());
        handledMessages_.pop_front();
        return true;
    }

    size_t pendingHandledMessageCount() const {
        std::lock_guard<std::mutex> lock(handledMessagesMutex_);
        return handledMessages_.size();
    }

protected:
    explicit TestFullClient(rsp::KeyPair keyPair) : rsp::client::full::RSPClient(std::move(keyPair)) {
    }

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override {
        if (message.has_ping_reply() || message.has_error()) {
            std::lock_guard<std::mutex> lock(handledMessagesMutex_);
            handledMessages_.push_back(message);
            return true;
        }

        return rsp::client::full::RSPClient::handleNodeSpecificMessage(message);
    }

private:
    mutable std::mutex handledMessagesMutex_;
    std::deque<rsp::proto::RSPMessage> handledMessages_;
};

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

using TraceEventTimes = std::unordered_map<std::string, rsp::ping_trace::Clock::time_point>;

TraceEventTimes indexTraceEvents(const rsp::ping_trace::TraceSnapshot& snapshot) {
    TraceEventTimes eventTimes;
    for (const auto& event : snapshot.events) {
        eventTimes.emplace(event.name, event.timestamp);
    }

    return eventTimes;
}

const rsp::ping_trace::Clock::time_point& requireEventTime(const TraceEventTimes& eventTimes,
                                                           const std::string& eventName) {
    const auto iterator = eventTimes.find(eventName);
    if (iterator == eventTimes.end()) {
        throw std::runtime_error("missing trace event: " + eventName);
    }

    return iterator->second;
}

double elapsedMilliseconds(const rsp::ping_trace::Clock::time_point& start,
                           const rsp::ping_trace::Clock::time_point& end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double addBreakdownSegment(PingBreakdown& breakdown,
                           const TraceEventTimes& eventTimes,
                           const std::string& label,
                           const std::string& startEvent,
                           const std::string& endEvent) {
    const double duration = elapsedMilliseconds(requireEventTime(eventTimes, startEvent),
                                                requireEventTime(eventTimes, endEvent));
    breakdown.segments.emplace_back(label, duration);
    return duration;
}

PingBreakdown buildPingBreakdown(const rsp::ping_trace::TraceSnapshot& snapshot) {
    const TraceEventTimes eventTimes = indexTraceEvents(snapshot);
    PingBreakdown breakdown;

    breakdown.handlerMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source call setup", "source_ping_call_start", "source_request_send_enqueued");

    breakdown.threadWakeupMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source send queue wait", "source_request_send_enqueued", "source_request_send_worker_start");
    breakdown.handlerMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source send worker dispatch", "source_request_send_worker_start", "source_request_serialize_start");
    breakdown.serializationMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source serialize", "source_request_serialize_start", "source_request_serialize_done");
    breakdown.transmissionMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source socket send", "source_request_serialize_done", "source_request_transport_send_done");
    breakdown.transmissionMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source to RM receive", "source_request_transport_send_done", "rm_request_read_complete");
    breakdown.handlerMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "RM receive enqueue", "rm_request_read_complete", "rm_request_received_queue_enqueued");
    breakdown.threadWakeupMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "RM route and forward send queue wait", "rm_request_received_queue_enqueued", "rm_forward_request_send_worker_start");
    breakdown.handlerMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "RM forward send worker dispatch", "rm_forward_request_send_worker_start", "rm_forward_request_serialize_start");
    breakdown.serializationMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "RM forward serialize", "rm_forward_request_serialize_start", "rm_forward_request_serialize_done");
    breakdown.transmissionMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "RM forward socket send", "rm_forward_request_serialize_done", "rm_forward_request_transport_send_done");
    breakdown.transmissionMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "RM to destination receive", "rm_forward_request_transport_send_done", "destination_request_read_complete");
    breakdown.handlerMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "destination receive enqueue", "destination_request_read_complete", "destination_request_received_queue_enqueued");
    breakdown.threadWakeupMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "destination client poll wait", "destination_request_received_queue_enqueued", "destination_client_poll_dequeue");
    breakdown.handlerMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "destination local handling to reply enqueue", "destination_client_poll_dequeue", "destination_reply_send_enqueued");
    breakdown.threadWakeupMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "destination reply send queue wait", "destination_reply_send_enqueued", "destination_reply_send_worker_start");
    breakdown.handlerMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "destination reply send worker dispatch", "destination_reply_send_worker_start", "destination_reply_serialize_start");
    breakdown.serializationMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "destination reply serialize", "destination_reply_serialize_start", "destination_reply_serialize_done");
    breakdown.transmissionMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "destination reply socket send", "destination_reply_serialize_done", "destination_reply_transport_send_done");
    breakdown.transmissionMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "destination to source receive", "destination_reply_transport_send_done", "source_reply_read_complete");
    breakdown.handlerMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source reply enqueue", "source_reply_read_complete", "source_reply_received_queue_enqueued");
    breakdown.threadWakeupMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source client poll wait", "source_reply_received_queue_enqueued", "source_client_poll_dequeue");
    breakdown.handlerMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source local handling to reply completion", "source_client_poll_dequeue", "source_ping_reply_completed");
    breakdown.threadWakeupMilliseconds += addBreakdownSegment(
        breakdown, eventTimes, "source waiter wakeup", "source_ping_reply_completed", "source_ping_wait_completed");

    breakdown.totalMilliseconds = elapsedMilliseconds(requireEventTime(eventTimes, "source_ping_call_start"),
                                                      requireEventTime(eventTimes, "source_ping_wait_completed"));
    return breakdown;
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

void requireMessageSender(const rsp::proto::RSPMessage& message,
                          const rsp::NodeID& expectedSender,
                          const std::string& context) {
    const auto senderNodeId = rsp::senderNodeIdFromMessage(message);
    require(senderNodeId.has_value(), context + " should identify a sender");
    require(senderNodeId.value() == expectedSender, context + " should identify the expected sender");
}

std::string findListeningEndpoint(const std::shared_ptr<rsp::transport::TcpTransport>& serverTransport) {
    if (!serverTransport->listen("127.0.0.1:0")) {
        throw std::runtime_error("failed to listen on a random port for handshake test");
    }

    return std::string("127.0.0.1:") + std::to_string(serverTransport->listenedPort());
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
        client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
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
    requireMessageSender(pingReply, resourceManager.nodeId(),
        "ping reply");
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

    void testFullClientUsesQueuedHandshakeAndSigningPipeline() {
        auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
        TestResourceManager resourceManager({serverTransport});

        rsp::KeyPair clientKeyPair = rsp::KeyPair::generateP256();
        const rsp::NodeID clientNodeId = clientKeyPair.nodeID();

        serverTransport->listen("rm-full-client-test");
        const std::string transportSpec = "memory:rm-full-client-test";

        auto client = TestFullClient::create(std::move(clientKeyPair));
        const auto connectionId =
        client->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

        require(client->hasConnections(), "full client should track created connections");
        require(client->hasConnection(connectionId), "full client should expose the new connection id");
        require(client->connectionCount() == 1, "full client should report one live connection");
        require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 1; }),
            "resource manager should authenticate the full client connection");

        rsp::proto::RSPMessage pingRequest;
        setNodeIdBytes(pingRequest.mutable_destination()->mutable_value(), resourceManager.nodeId());
            pingRequest.mutable_ping_request()->mutable_nonce()->set_value("full-client-ping");
        pingRequest.mutable_ping_request()->set_sequence(19);
        pingRequest.mutable_ping_request()->mutable_time_sent()->set_milliseconds_since_epoch(987654321);

        require(client->sendOnConnection(connectionId, pingRequest),
            "full client should enqueue outbound messages through its signing queue");
            require(waitForCondition([&client]() { return client->pendingHandledMessageCount() == 1; }),
                "full client should receive a reply from the resource manager through the verified queue pipeline");

            rsp::proto::RSPMessage reply;
            require(client->tryDequeueHandledMessage(reply),
                "full client test harness should capture the handled reply");
            require(reply.has_ping_reply(),
                "resource manager should answer the full client's ping request");
            require(reply.has_nonce() && !reply.nonce().value().empty(),
                "full client pipeline should populate a top-level message nonce that survives the round trip");
            require(reply.has_destination(), "resource manager ping reply should identify the destination");

            rsp::proto::NodeId expectedClientSource;
            setNodeIdBytes(expectedClientSource.mutable_value(), clientNodeId);
            require(reply.destination().value() == expectedClientSource.value(),
                "resource manager reply should target the full client's node id");
            requireMessageSender(reply, resourceManager.nodeId(),
                "resource manager reply");
            require(reply.ping_reply().nonce().value() == pingRequest.ping_request().nonce().value(),
                "resource manager reply should preserve the nested ping nonce");

        require(client->removeConnection(connectionId), "full client should remove an existing connection");
        serverTransport->stop();
    }

ClientToClientPingResults testClientToClientRouting() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair firstClientKeyPair = rsp::KeyPair::generateP256();
    rsp::KeyPair secondClientKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID secondClientNodeId = secondClientKeyPair.nodeID();

    serverTransport->listen("rm-test");
    const std::string transportSpec = "memory:rm-test";

    rsp::client::RSPClient::Ptr firstClient = rsp::client::RSPClient::create(std::move(firstClientKeyPair));
    rsp::client::RSPClient::Ptr secondClient = rsp::client::RSPClient::create(std::move(secondClientKeyPair));

    const rsp::client::RSPClient::ClientConnectionID firstConnectionId =
        firstClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
    const rsp::client::RSPClient::ClientConnectionID secondConnectionId =
        secondClient->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);

    require(firstClient->hasConnections(), "high-level client should track created connections");
    require(firstClient->hasConnection(firstConnectionId), "high-level client should expose existing connection ids");
    require(firstClient->connectionCount() == 1, "high-level client should report one live connection");
    require(firstClient->connectionIds().size() == 1, "high-level client should enumerate its connections");
    require(secondClient->hasConnection(secondConnectionId), "second high-level client should expose its connection id");

    require(waitForCondition([&resourceManager]() { return resourceManager.activeEncodingCount() == 2; }),
        "resource manager should authenticate both client connections");

    std::vector<double> roundTripDelayMilliseconds;
    roundTripDelayMilliseconds.reserve(100);

    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto pingStart = std::chrono::steady_clock::now();
        require(firstClient->ping(secondClientNodeId), "high-level client should route a ping to another client through the resource manager");
        const auto pingEnd = std::chrono::steady_clock::now();
        roundTripDelayMilliseconds.push_back(
            std::chrono::duration<double, std::milli>(pingEnd - pingStart).count());
    }

    const auto [minimumIterator, maximumIterator] =
        std::minmax_element(roundTripDelayMilliseconds.begin(), roundTripDelayMilliseconds.end());
    double totalMilliseconds = 0.0;
    for (const double sampleMilliseconds : roundTripDelayMilliseconds) {
        totalMilliseconds += sampleMilliseconds;
    }

    const PingStats stats{
        *minimumIterator,
        totalMilliseconds / static_cast<double>(roundTripDelayMilliseconds.size()),
        *maximumIterator,
    };

    rsp::ping_trace::reset();
    rsp::ping_trace::setEnabled(true);
    require(firstClient->ping(secondClientNodeId), "instrumented ping should complete successfully");
    rsp::ping_trace::setEnabled(false);

    const auto traces = rsp::ping_trace::snapshotAll();
    require(traces.size() == 1, "instrumented ping should produce exactly one trace snapshot");
    const PingBreakdown breakdown = buildPingBreakdown(traces.front());

    require(firstClient->removeConnection(firstConnectionId), "high-level client should remove an existing connection");
    require(secondClient->removeConnection(secondConnectionId), "second high-level client should remove an existing connection");

    return ClientToClientPingResults{stats, breakdown};
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
        testFullClientUsesQueuedHandshakeAndSigningPipeline();
        const ClientToClientPingResults clientToClientPingResults = testClientToClientRouting();

                std::cout << "client-to-client ping round-trip stats over 100 pings: min="
                                    << clientToClientPingResults.stats.minimumMilliseconds
                                    << " ms avg="
                                    << clientToClientPingResults.stats.averageMilliseconds
                                    << " ms max="
                                    << clientToClientPingResults.stats.maximumMilliseconds
                                    << " ms\n";

                std::cout << "instrumented steady-state ping breakdown: total="
                                    << clientToClientPingResults.breakdown.totalMilliseconds
                                    << " ms serialization="
                                    << clientToClientPingResults.breakdown.serializationMilliseconds
                                    << " ms thread_wakeup="
                                    << clientToClientPingResults.breakdown.threadWakeupMilliseconds
                                    << " ms transmission="
                                    << clientToClientPingResults.breakdown.transmissionMilliseconds
                                    << " ms handler="
                                    << clientToClientPingResults.breakdown.handlerMilliseconds
                                    << " ms\n";

                for (const auto& [label, duration] : clientToClientPingResults.breakdown.segments) {
                        std::cout << "  " << label << ": " << duration << " ms\n";
                }

        std::cout << "client_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "client_test failed: " << exception.what() << '\n';
        return 1;
    }
}