#pragma once

#include "client/cpp/rsp_client_export.hpp"
#include "client/cpp/rsp_client_message.hpp"
#include "common/node.hpp"
#include "name_service/name_service.pb.h"
#include "os/os_socket.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
    RSPCLIENT_API std::optional<rsp::NodeID> peerNodeID(ClientConnectionID connectionId) const;

    RSPCLIENT_API bool ping(rsp::NodeID nodeId);
    RSPCLIENT_API std::optional<rsp::proto::EndorsementDone> beginEndorsementRequest(
        rsp::NodeID nodeId,
        const rsp::GUID& endorsementType,
        const rsp::Buffer& endorsementValue = rsp::Buffer());
    RSPCLIENT_API bool queryResources(rsp::NodeID nodeId,
                                      const std::string& query = std::string(),
                                      uint32_t maxRecords = 0);
    RSPCLIENT_API std::optional<rsp::proto::ResourceQueryReply> resourceList(
        rsp::NodeID nodeId,
        const std::string& query = std::string(),
        uint32_t maxRecords = 0);

    RSPCLIENT_API std::optional<rsp::proto::NameCreateReply> nameCreate(
        rsp::NodeID nodeId,
        const std::string& name,
        rsp::NodeID owner,
        const rsp::GUID& type,
        const rsp::GUID& value);
    RSPCLIENT_API std::optional<rsp::proto::NameReadReply> nameRead(
        rsp::NodeID nodeId,
        const std::string& name,
        const std::optional<rsp::NodeID>& owner = std::nullopt,
        const std::optional<rsp::GUID>& type = std::nullopt);
    RSPCLIENT_API std::optional<rsp::proto::NameUpdateReply> nameUpdate(
        rsp::NodeID nodeId,
        const std::string& name,
        rsp::NodeID owner,
        const rsp::GUID& type,
        const rsp::GUID& newValue);
    RSPCLIENT_API std::optional<rsp::proto::NameDeleteReply> nameDelete(
        rsp::NodeID nodeId,
        const std::string& name,
        rsp::NodeID owner,
        const rsp::GUID& type);
    RSPCLIENT_API std::optional<rsp::proto::NameQueryReply> nameQuery(
        rsp::NodeID nodeId,
        const std::string& namePrefix = std::string(),
        const std::optional<rsp::NodeID>& owner = std::nullopt,
        const std::optional<rsp::GUID>& type = std::nullopt,
        uint32_t maxRecords = 0);

    RSPCLIENT_API std::optional<rsp::proto::StreamReply> connectTCPEx(rsp::NodeID nodeId,
                                                                         const std::string& hostPort,
                                                                         uint32_t timeoutMilliseconds = 0,
                                                                         uint32_t retries = 0,
                                                                         uint32_t retryMilliseconds = 0,
                                                                         bool asyncData = false,
                                                                         bool shareSocket = false,
                                                                         bool useSocket = false);
    RSPCLIENT_API std::optional<rsp::GUID> connectTCP(rsp::NodeID nodeId,
                                                      const std::string& hostPort,
                                                      uint32_t timeoutMilliseconds = 0,
                                                      uint32_t retries = 0,
                                                      uint32_t retryMilliseconds = 0,
                                                      bool asyncData = false,
                                                      bool shareSocket = false,
                                                      bool useSocket = false);
    RSPCLIENT_API std::optional<rsp::proto::StreamReply> listenTCPEx(rsp::NodeID nodeId,
                                                                     const std::string& hostPort,
                                                                     uint32_t timeoutMilliseconds = 0,
                                                                     bool asyncAccept = false,
                                                                     bool shareListeningSocket = false,
                                                                     bool shareChildSockets = false,
                                                                     bool childrenUseSocket = false,
                                                                     bool childrenAsyncData = false);
    RSPCLIENT_API std::optional<rsp::GUID> listenTCP(rsp::NodeID nodeId,
                                                     const std::string& hostPort,
                                                     uint32_t timeoutMilliseconds = 0,
                                                     bool asyncAccept = false,
                                                     bool shareListeningSocket = false,
                                                     bool shareChildSockets = false,
                                                     bool childrenUseSocket = false,
                                                     bool childrenAsyncData = false);
    RSPCLIENT_API std::optional<rsp::os::SocketHandle> listenTCPSocket(rsp::NodeID nodeId,
                                                                       const std::string& hostPort,
                                                                       uint32_t timeoutMilliseconds = 0);
    RSPCLIENT_API std::optional<rsp::proto::StreamReply> acceptTCPEx(const rsp::GUID& listenSocketId,
                                                                     const std::optional<rsp::GUID>& newSocketId = std::nullopt,
                                                                     uint32_t timeoutMilliseconds = 0,
                                                                     bool shareChildSocket = false,
                                                                     bool childUseSocket = false,
                                                                     bool childAsyncData = false);
    RSPCLIENT_API std::optional<rsp::GUID> acceptTCP(const rsp::GUID& listenSocketId,
                                                     const std::optional<rsp::GUID>& newSocketId = std::nullopt,
                                                     uint32_t timeoutMilliseconds = 0,
                                                     bool shareChildSocket = false,
                                                     bool childUseSocket = false,
                                                     bool childAsyncData = false);
    RSPCLIENT_API std::optional<rsp::os::SocketHandle> acceptTCPSocket(const rsp::GUID& listenSocketId,
                                                                       const std::optional<rsp::GUID>& newSocketId = std::nullopt,
                                                                       uint32_t timeoutMilliseconds = 0);
    RSPCLIENT_API std::optional<rsp::os::SocketHandle> connectTCPSocket(rsp::NodeID nodeId,
                                                                        const std::string& hostPort,
                                                                        uint32_t timeoutMilliseconds = 0,
                                                                        uint32_t retries = 0,
                                                                        uint32_t retryMilliseconds = 0);
    RSPCLIENT_API std::optional<rsp::proto::StreamReply> connectSshdEx(rsp::NodeID nodeId,
                                                                        uint32_t timeoutMilliseconds = 0,
                                                                        bool asyncData = false,
                                                                        bool shareSocket = false,
                                                                        bool useSocket = false);
    RSPCLIENT_API std::optional<rsp::GUID> connectSshd(rsp::NodeID nodeId,
                                                        uint32_t timeoutMilliseconds = 0,
                                                        bool asyncData = false,
                                                        bool shareSocket = false,
                                                        bool useSocket = false);
    RSPCLIENT_API std::optional<rsp::os::SocketHandle> connectSshdSocket(rsp::NodeID nodeId,
                                                                          uint32_t timeoutMilliseconds = 0);

    // Connect to an HTTP(S) server registered as an HttpdResourceService.
    // Returns the full StreamReply so callers can inspect error details.
    // On success the stream is registered in streamRoutes_ and subsequent
    // streamSend / streamRecv / streamClose calls work as normal.
    RSPCLIENT_API std::optional<rsp::proto::StreamReply> connectHttpEx(rsp::NodeID nodeId,
                                                                        uint32_t timeoutMilliseconds = 0,
                                                                        bool asyncData = false,
                                                                        bool shareSocket = false);
    // Convenience wrapper: returns the chosen stream GUID on success, nullopt on failure.
    RSPCLIENT_API std::optional<rsp::GUID> connectHttp(rsp::NodeID nodeId,
                                                        uint32_t timeoutMilliseconds = 0,
                                                        bool asyncData = false,
                                                        bool shareSocket = false);
    RSPCLIENT_API bool streamSend(const rsp::GUID& socketId, const std::string& data);
    RSPCLIENT_API std::optional<rsp::proto::StreamReply> streamRecvEx(const rsp::GUID& socketId,
                                                                         uint32_t maxBytes = 4096,
                                                                         uint32_t waitMilliseconds = 0);
    RSPCLIENT_API std::optional<std::string> streamRecv(const rsp::GUID& socketId,
                                                        uint32_t maxBytes = 4096,
                                                        uint32_t waitMilliseconds = 0);
    RSPCLIENT_API bool streamClose(const rsp::GUID& socketId);
    RSPCLIENT_API bool tryDequeueStreamReply(rsp::proto::StreamReply& reply);
    RSPCLIENT_API std::size_t pendingStreamReplyCount() const;
    RSPCLIENT_API bool tryDequeueResourceAdvertisement(rsp::proto::ResourceAdvertisement& advertisement);
    RSPCLIENT_API std::size_t pendingResourceAdvertisementCount() const;
    RSPCLIENT_API bool tryDequeueResourceQueryReply(rsp::proto::ResourceQueryReply& reply);
    RSPCLIENT_API std::size_t pendingResourceQueryReplyCount() const;
    RSPCLIENT_API bool querySchemas(rsp::NodeID nodeId,
                                    const std::string& protoFileName = std::string(),
                                    const std::string& schemaHash = std::string());
    RSPCLIENT_API bool tryDequeueSchemaReply(rsp::proto::SchemaReply& reply);
    RSPCLIENT_API std::size_t pendingSchemaReplyCount() const;
    RSPCLIENT_API void registerStreamRoute(const rsp::GUID& socketId, rsp::NodeID nodeId);

private:
    struct PendingPingState {
        rsp::NodeID destination;
        uint32_t sequence = 0;
        bool completed = false;
    };

    struct PendingConnectState {
        bool completed = false;
        std::optional<rsp::proto::StreamReply> reply;
    };

    struct PendingEndorsementState {
        rsp::NodeID destination;
        bool completed = false;
        std::optional<rsp::proto::EndorsementDone> reply;
    };

    struct NativeStreamBridgeState {
        rsp::os::SocketHandle bridgeSocket = 0;
        std::atomic<bool> stopping = false;
        std::atomic<bool> remoteClosed = false;
        std::thread worker;
    };

    struct NativeListenBridgeState {
        rsp::GUID listenSocketId;
        rsp::NodeID nodeId;
        std::string localEndpoint;
        std::atomic<bool> stopping = false;
        std::thread worker;
    };

    explicit RSPClient(KeyPair keyPair);

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    void handleOutputMessage(rsp::proto::RSPMessage message) override;
    void receiveLoop();
    void dispatchIncomingMessage(rsp::proto::RSPMessage message);
    bool shouldHandleLocally(const rsp::proto::RSPMessage& message) const;
    void handlePingReply(const rsp::proto::RSPMessage& message);
    void handleEndorsementDone(const rsp::proto::RSPMessage& message);
    void handleStreamReply(const rsp::proto::RSPMessage& message);
    void handleResourceAdvertisement(const rsp::proto::RSPMessage& message);
    void handleResourceQueryReply(const rsp::proto::RSPMessage& message);
    void handleSchemaReply(const rsp::proto::RSPMessage& message);
    void handleNameServiceReply(const rsp::proto::RSPMessage& message);
    bool sendIdentity(rsp::NodeID nodeId);
    bool sendBeginEndorsementRequestMessage(rsp::NodeID nodeId,
                                            const rsp::proto::Endorsement& requestedMessage);
    std::optional<rsp::proto::EndorsementDone> waitForPendingEndorsement(const std::string& pendingKey);
    std::shared_ptr<NativeStreamBridgeState> attachNativeStreamBridge(const rsp::GUID& socketId,
                                                                      rsp::os::SocketHandle bridgeSocket);
    void startNativeStreamBridgeWorker(const rsp::GUID& socketId,
                                       const std::shared_ptr<NativeStreamBridgeState>& bridgeState);
    void runNativeStreamBridge(const rsp::GUID& socketId,
                               const std::shared_ptr<NativeStreamBridgeState>& bridgeState);
    void runNativeListenSocketBridge(const std::shared_ptr<NativeListenBridgeState>& bridgeState);
    void stopNativeSocketBridges();
    void stopNativeSocketBridgesForNode(const rsp::NodeID& nodeId);
    void stopNativeListenBridges();
    void stopNativeListenBridgesForNode(const rsp::NodeID& nodeId);
    static rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId);
    static rsp::proto::Uuid toProtoUuid(const rsp::GUID& guid);
    static rsp::proto::StreamID toProtoStreamId(const rsp::GUID& socketId);
    static std::optional<rsp::GUID> fromProtoStreamId(const rsp::proto::StreamID& socketId);
    std::optional<rsp::proto::StreamReply> waitForStreamReply(const rsp::GUID& socketId);

    RSPClientMessage::Ptr messageClient_;
    std::thread receiveThread_;
    mutable std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    std::map<std::string, PendingPingState> pendingPings_;
    std::map<std::string, PendingEndorsementState> pendingEndorsements_;
    std::map<rsp::GUID, PendingConnectState> pendingConnects_;
    std::map<rsp::GUID, PendingConnectState> pendingListens_;
    std::deque<rsp::proto::StreamReply> pendingStreamReplies_;
    std::deque<rsp::proto::ResourceAdvertisement> pendingResourceAdvertisements_;
    std::deque<rsp::proto::ResourceQueryReply> pendingResourceQueryReplies_;
    bool resourceListPending_ = false;
    std::optional<rsp::proto::ResourceQueryReply> resourceListResult_;
    bool nameReplyPending_ = false;
    std::optional<rsp::proto::RSPMessage> nameReplyMessage_;
    std::deque<rsp::proto::SchemaReply> pendingSchemaReplies_;
    std::map<rsp::GUID, std::deque<rsp::proto::StreamReply>> streamReplyQueues_;
    std::set<rsp::GUID> awaitedStreamReplies_;
    std::map<rsp::GUID, std::shared_ptr<NativeStreamBridgeState>> nativeStreamBridges_;
    std::map<rsp::GUID, std::shared_ptr<NativeListenBridgeState>> nativeListenBridges_;
    std::map<rsp::GUID, rsp::NodeID> streamRoutes_;
    uint32_t nextPingSequence_ = 1;
    bool stopping_ = false;
};

}  // namespace rsp::client