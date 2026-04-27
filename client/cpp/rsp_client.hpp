#pragma once

#include "client/cpp/rsp_client_export.hpp"
#include "client/cpp/rsp_client_message.hpp"
#include "common/base_types.hpp"
#include "common/endorsement/endorsement.hpp"
#include "common/node.hpp"
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

// Mirrors proto STREAM_STATUS enum values exactly
enum class StreamStatus {
    Success = 0,
    ConnectRefused = 1,
    ConnectTimeout = 2,
    Closed = 3,
    Data = 4,
    Error = 5,
    NewConnection = 6,
    Async = 7,
    NodeIdMismatch = 9,
    InvalidFlags = 10,
    InUse = 11,
    TimedOut = 12,
};

struct StreamResult {
    StreamStatus status = StreamStatus::Error;
    rsp::GUID streamId;
    rsp::GUID newStreamId;
    bool hasNewStreamId = false;
    std::string data;
    std::string message;
    int errorCode = 0;
};

struct NameRecord {
    std::string name;
    rsp::NodeID owner;
    rsp::GUID type;
    rsp::GUID value;
    rsp::DateTime expiresAt;
};

struct NameResult {
    enum class Status { Success = 0, NotFound = 1, Duplicate = 2, Error = 3 };
    Status status = Status::Success;
    std::string message;
    std::vector<NameRecord> records;
};

struct DiscoveredService {
    rsp::NodeID nodeId;
    std::string protoFileName;
    std::vector<std::string> acceptedTypeUrls;
};

struct ResourceQueryResult {
    bool success = false;
    std::vector<DiscoveredService> services;
};

struct ResourceAdvertisement {
    std::vector<DiscoveredService> services;
};

struct SchemaInfo {
    std::string protoFileName;
    std::string descriptorSet;
    std::vector<std::string> acceptedTypeUrls;
    uint32_t schemaVersion = 0;
};

struct EndorsementResult {
    enum class Status { Success = 0, Failed = 1, Challenge = 2, InvalidSignature = 3, UnknownIdentity = 4 };
    Status status = Status::Failed;
    std::optional<rsp::Endorsement> newEndorsement;
};

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

    RSPCLIENT_API std::optional<ClientConnectionID> connectToResourceManager(const std::string& transport, const std::string& encoding);
    RSPCLIENT_API bool hasConnections() const;
    RSPCLIENT_API bool hasConnection(ClientConnectionID connectionId) const;
    RSPCLIENT_API std::size_t connectionCount() const;
    RSPCLIENT_API std::vector<ClientConnectionID> connectionIds() const;
    RSPCLIENT_API bool removeConnection(ClientConnectionID connectionId);
    RSPCLIENT_API std::optional<rsp::NodeID> peerNodeID(ClientConnectionID connectionId) const;

    RSPCLIENT_API bool ping(rsp::NodeID nodeId);
    RSPCLIENT_API std::optional<EndorsementResult> beginEndorsementRequest(
        rsp::NodeID nodeId,
        const rsp::GUID& endorsementType,
        const rsp::Buffer& endorsementValue = rsp::Buffer());
    RSPCLIENT_API bool queryResources(rsp::NodeID nodeId,
                                      const std::string& query = std::string(),
                                      uint32_t maxRecords = 0);
    RSPCLIENT_API std::optional<ResourceQueryResult> resourceList(
        rsp::NodeID nodeId,
        const std::string& query = std::string(),
        uint32_t maxRecords = 0);

    RSPCLIENT_API std::optional<NameResult> nameCreate(
        rsp::NodeID nodeId,
        const std::string& name,
        rsp::NodeID owner,
        const rsp::GUID& type,
        const rsp::GUID& value);
    RSPCLIENT_API std::optional<NameResult> nameRead(
        rsp::NodeID nodeId,
        const std::string& name,
        const std::optional<rsp::NodeID>& owner = std::nullopt,
        const std::optional<rsp::GUID>& type = std::nullopt);
    RSPCLIENT_API std::optional<NameResult> nameUpdate(
        rsp::NodeID nodeId,
        const std::string& name,
        rsp::NodeID owner,
        const rsp::GUID& type,
        const rsp::GUID& newValue);
    RSPCLIENT_API std::optional<NameResult> nameDelete(
        rsp::NodeID nodeId,
        const std::string& name,
        rsp::NodeID owner,
        const rsp::GUID& type);
    RSPCLIENT_API std::optional<NameResult> nameQuery(
        rsp::NodeID nodeId,
        const std::string& namePrefix = std::string(),
        const std::optional<rsp::NodeID>& owner = std::nullopt,
        const std::optional<rsp::GUID>& type = std::nullopt,
        uint32_t maxRecords = 0);
    RSPCLIENT_API std::optional<NameResult> nameRefresh(
        rsp::NodeID nodeId,
        const std::string& name,
        rsp::NodeID owner,
        const rsp::GUID& type);
    RSPCLIENT_API std::optional<rsp::NodeID> nameResolve(
        rsp::NodeID nsNodeId,
        const std::string& name,
        const std::optional<rsp::GUID>& type = std::nullopt);
    RSPCLIENT_API std::optional<NameResult> registerNameWithRefresh(
        rsp::NodeID nsNodeId,
        const std::string& name,
        rsp::NodeID owner,
        const rsp::GUID& type,
        const rsp::GUID& value);
    // Watch for any node connecting to RM and call |callback| when one does.
    // Also subscribes to NodeConnectedEvent if not already subscribed.
    // Used when no NS was found at startup — the callback can query the RM and
    // call registerNameWithRefresh when it discovers an NS.
    RSPCLIENT_API void watchNodeConnectedEvents(std::function<void(rsp::NodeID)> callback);

    RSPCLIENT_API std::optional<StreamResult> connectTCPEx(rsp::NodeID nodeId,
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
    RSPCLIENT_API std::optional<StreamResult> listenTCPEx(rsp::NodeID nodeId,
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
    RSPCLIENT_API std::optional<StreamResult> acceptTCPEx(const rsp::GUID& listenSocketId,
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
    RSPCLIENT_API std::optional<StreamResult> connectSshdEx(rsp::NodeID nodeId,
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

    RSPCLIENT_API std::optional<StreamResult> connectHttpEx(rsp::NodeID nodeId,
                                                            uint32_t timeoutMilliseconds = 0,
                                                            bool asyncData = false,
                                                            bool shareSocket = false);
    RSPCLIENT_API std::optional<rsp::GUID> connectHttp(rsp::NodeID nodeId,
                                                        uint32_t timeoutMilliseconds = 0,
                                                        bool asyncData = false,
                                                        bool shareSocket = false);
    RSPCLIENT_API bool streamSend(const rsp::GUID& socketId, const std::string& data);
    RSPCLIENT_API std::optional<StreamResult> streamRecvEx(const rsp::GUID& socketId,
                                                           uint32_t maxBytes = 4096,
                                                           uint32_t waitMilliseconds = 0);
    RSPCLIENT_API std::optional<std::string> streamRecv(const rsp::GUID& socketId,
                                                        uint32_t maxBytes = 4096,
                                                        uint32_t waitMilliseconds = 0);
    RSPCLIENT_API bool streamClose(const rsp::GUID& socketId);
    RSPCLIENT_API bool tryDequeueStreamResult(StreamResult& result);
    RSPCLIENT_API std::size_t pendingStreamReplyCount() const;
    RSPCLIENT_API bool tryDequeueResourceAdvertisement(ResourceAdvertisement& advertisement);
    RSPCLIENT_API std::size_t pendingResourceAdvertisementCount() const;
    RSPCLIENT_API bool tryDequeueResourceQueryReply(ResourceQueryResult& reply);
    RSPCLIENT_API std::size_t pendingResourceQueryReplyCount() const;
    RSPCLIENT_API bool querySchemas(rsp::NodeID nodeId,
                                    const std::string& protoFileName = std::string(),
                                    const std::string& schemaHash = std::string());
    RSPCLIENT_API bool tryDequeueSchemaReply(SchemaInfo& reply);
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
        StreamResult result;
    };

    struct PendingEndorsementState {
        rsp::NodeID destination;
        bool completed = false;
        std::optional<EndorsementResult> result;
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
    std::optional<EndorsementResult> waitForPendingEndorsement(const std::string& pendingKey);
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
    std::optional<StreamResult> waitForStreamReply(const rsp::GUID& socketId);

    RSPClientMessage::Ptr messageClient_;
    std::thread receiveThread_;
    mutable std::mutex stateMutex_;
    mutable std::condition_variable stateChanged_;
    std::map<std::string, PendingPingState> pendingPings_;
    std::map<std::string, PendingEndorsementState> pendingEndorsements_;
    std::map<rsp::GUID, PendingConnectState> pendingConnects_;
    std::map<rsp::GUID, PendingConnectState> pendingListens_;
    std::deque<StreamResult> pendingStreamResults_;
    std::deque<ResourceAdvertisement> pendingResourceAdvertisements_;
    std::deque<ResourceQueryResult> pendingResourceQueryReplies_;
    bool resourceListPending_ = false;
    std::optional<ResourceQueryResult> resourceListResult_;
    bool nameReplyPending_ = false;
    std::optional<NameResult> nameReplyResult_;
    std::deque<SchemaInfo> pendingSchemaReplies_;
    std::map<rsp::GUID, std::deque<StreamResult>> streamReplyQueues_;
    std::set<rsp::GUID> awaitedStreamReplies_;
    std::map<rsp::GUID, std::shared_ptr<NativeStreamBridgeState>> nativeStreamBridges_;
    std::map<rsp::GUID, std::shared_ptr<NativeListenBridgeState>> nativeListenBridges_;
    std::map<rsp::GUID, rsp::NodeID> streamRoutes_;
    uint32_t nextPingSequence_ = 1;
    bool stopping_ = false;

    struct RefreshEntry {
        rsp::NodeID nsNodeId;
        std::string name;
        rsp::NodeID owner;
        rsp::GUID type;
        rsp::GUID value;
    };
    std::vector<RefreshEntry> refreshRegistrations_;
    std::map<rsp::NodeID, std::string> nsBootIds_;    // last known boot_id per NS node
    std::set<rsp::NodeID> pendingReregistrations_;    // NSes that need re-registration
    std::set<rsp::NodeID> failedRegistrations_;       // NSes with transient failures, retry soon
    std::function<void(rsp::NodeID)> nodeConnectedCallback_; // called for unknown connecting nodes
    bool logSubActive_ = false;
    std::mutex refreshMutex_;
    std::condition_variable refreshCv_;
    bool refreshStopping_ = false;
    std::thread refreshThread_;
    void runRefreshThread();
    void sendLogSubscribeToRM();
    void handleLogRecord(const rsp::proto::RSPMessage& message);
};

}  // namespace rsp::client