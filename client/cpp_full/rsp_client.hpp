#pragma once

#include "common/encoding/encoding.hpp"
#include "common/message_queue/mq.hpp"
#include "common/node.hpp"
#include "common/transport/transport.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rsp::client::full {

class RSPClient : public rsp::RSPNode {
public:
    using Ptr = std::shared_ptr<RSPClient>;
    using ConstPtr = std::shared_ptr<const RSPClient>;
    using ClientConnectionID = rsp::GUID;

    static Ptr create();
    static Ptr create(KeyPair keyPair);

    ~RSPClient() override;

    RSPClient(const RSPClient&) = delete;
    RSPClient& operator=(const RSPClient&) = delete;
    RSPClient(RSPClient&&) = delete;
    RSPClient& operator=(RSPClient&&) = delete;

    int run() const override;
    void stop();

    ClientConnectionID connectToResourceManager(const std::string& transportSpec, const std::string& encoding);
    bool send(const rsp::proto::RSPMessage& message) const;
    bool sendOnConnection(ClientConnectionID connectionId, const rsp::proto::RSPMessage& message) const;
    bool hasConnections() const;
    bool hasConnection(ClientConnectionID connectionId) const;
    std::size_t connectionCount() const;
    std::vector<ClientConnectionID> connectionIds() const;
    bool removeConnection(ClientConnectionID connectionId);
    std::optional<rsp::NodeID> peerNodeID(ClientConnectionID connectionId) const;
    rsp::NodeID nodeId() const;
    void dispatchIncomingMessage(rsp::proto::RSPMessage message);

    bool ping(rsp::NodeID nodeId, uint32_t timeoutMs = 2000);

protected:
    explicit RSPClient(KeyPair keyPair);

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    void handleOutputMessage(rsp::proto::RSPMessage message) override;
    virtual rsp::transport::TransportHandle createTransport(const std::string& transportName) const;
    rsp::proto::RSPMessage prepareOutboundMessage(const rsp::proto::RSPMessage& message) const;

private:
    struct ClientConnectionState {
        rsp::transport::TransportHandle transport;
        rsp::encoding::EncodingHandle encoding;
        rsp::MessageQueueHandle signingQueue;
    };

    struct PendingPingState {
        rsp::NodeID destination;
        uint32_t sequence = 0;
        bool completed = false;
    };

    bool isForThisNode(const rsp::proto::RSPMessage& message) const;
    std::optional<ClientConnectionState> connectionState(ClientConnectionID connectionId) const;
    void handlePingReply(const rsp::proto::RSPMessage& message);

    mutable std::mutex connectionsMutex_;
    std::map<ClientConnectionID, ClientConnectionState> connections_;
    rsp::MessageQueueHandle incomingMessages_;
    mutable std::mutex runMutex_;
    mutable std::condition_variable runCondition_;
    bool stopping_ = false;

    mutable std::mutex pingMutex_;
    std::condition_variable pingCv_;
    std::map<std::string, PendingPingState> pendingPings_;
    uint32_t nextPingSequence_ = 1;
};

}  // namespace rsp::client::full