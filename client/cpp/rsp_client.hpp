#pragma once

#include "client/cpp/rsp_client_export.hpp"
#include "client/cpp/rsp_client_message.hpp"
#include "common/node.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rsp::client {

class RSPClient : public rsp::RSPNode, public std::enable_shared_from_this<RSPClient> {
public:
    using Ptr = std::shared_ptr<RSPClient>;
    using ConstPtr = std::shared_ptr<const RSPClient>;
    using ClientConnectionID = RSPClientMessage::ClientConnectionID;

    RSPCLIENT_API static Ptr create();
    RSPCLIENT_API static Ptr create(KeyPair keyPair);

    RSPCLIENT_API ~RSPClient();

    RSPClient(const RSPClient&) = delete;
    RSPClient& operator=(const RSPClient&) = delete;
    RSPClient(RSPClient&&) = delete;
    RSPClient& operator=(RSPClient&&) = delete;

    RSPCLIENT_API int run() const override;

    RSPCLIENT_API ClientConnectionID connectToResourceManager(const std::string& transport, const std::string& encoding);
    RSPCLIENT_API bool hasConnections() const;
    RSPCLIENT_API bool hasConnection(ClientConnectionID connectionId) const;
    RSPCLIENT_API std::size_t connectionCount() const;
    RSPCLIENT_API std::vector<ClientConnectionID> connectionIds() const;
    RSPCLIENT_API bool removeConnection(ClientConnectionID connectionId);
    RSPCLIENT_API bool ping(rsp::NodeID nodeId);
    RSPCLIENT_API std::optional<rsp::GUID> connectTCP(rsp::NodeID nodeId,
                                                      const std::string& hostPort,
                                                      uint32_t timeoutMilliseconds = 0,
                                                      uint32_t retries = 0,
                                                      uint32_t retryMilliseconds = 0);
    RSPCLIENT_API bool socketSend(const rsp::GUID& socketId, const std::string& data);
    RSPCLIENT_API std::optional<std::string> socketRecv(const rsp::GUID& socketId,
                                                        uint32_t maxBytes = 4096,
                                                        uint32_t waitMilliseconds = 0);
    RSPCLIENT_API bool socketClose(const rsp::GUID& socketId);

private:
    struct PendingPingState {
        rsp::NodeID destination;
        uint32_t sequence = 0;
        bool completed = false;
    };

    struct PendingConnectState {
        bool completed = false;
        std::optional<rsp::GUID> socketId;
    };

    explicit RSPClient(KeyPair keyPair);

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    void handleOutputMessage(rsp::proto::RSPMessage message) override;
    void receiveLoop();
    void dispatchIncomingMessage(rsp::proto::RSPMessage message);
    bool shouldHandleLocally(const rsp::proto::RSPMessage& message) const;
    void handlePingReply(const rsp::proto::RSPMessage& message);
    void handleConnectTCPReply(const rsp::proto::RSPMessage& message);
    void handleSocketReply(const rsp::proto::RSPMessage& message);
    static rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId);
    static rsp::proto::SocketID toProtoSocketId(const rsp::GUID& socketId);
    static std::optional<rsp::GUID> fromProtoSocketId(const rsp::proto::SocketID& socketId);

    RSPClientMessage::Ptr messageClient_;
    std::thread receiveThread_;
    mutable std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    std::map<std::string, PendingPingState> pendingPings_;
    std::optional<PendingConnectState> pendingConnect_;
    std::deque<rsp::proto::SocketReply> pendingSocketReplies_;
    std::map<rsp::GUID, rsp::NodeID> socketRoutes_;
    uint32_t nextPingSequence_ = 1;
    bool stopping_ = false;
};

}  // namespace rsp::client