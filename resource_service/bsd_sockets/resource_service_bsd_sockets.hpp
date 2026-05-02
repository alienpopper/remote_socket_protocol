#pragma once

#include "resource_service/resource_service.hpp"
#include "common/message_queue/mq.hpp"

#include <google/protobuf/message.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rsp::resource_service {

class BsdSocketsResourceService : public ResourceService {
public:
    using Ptr = std::shared_ptr<BsdSocketsResourceService>;

    static Ptr create();
    static Ptr create(KeyPair keyPair);

    ~BsdSocketsResourceService() override;

    BsdSocketsResourceService(const BsdSocketsResourceService&) = delete;
    BsdSocketsResourceService& operator=(const BsdSocketsResourceService&) = delete;
    BsdSocketsResourceService(BsdSocketsResourceService&&) = delete;
    BsdSocketsResourceService& operator=(BsdSocketsResourceService&&) = delete;

protected:
    explicit BsdSocketsResourceService(KeyPair keyPair);

    rsp::proto::ResourceAdvertisement buildResourceAdvertisement() const override;

    virtual bool handleListenTCPRequest(const rsp::proto::RSPMessage& message);

    struct TCPConnectionResult {
        rsp::transport::TransportHandle transport;
        rsp::transport::ConnectionHandle connection;
    };

    virtual TCPConnectionResult createTCPConnection(const std::string& hostPort,
                                                    uint32_t totalAttempts,
                                                    uint32_t retryDelayMs);

    bool registerConnectedSocket(const rsp::proto::RSPMessage& message,
                                 TCPConnectionResult&& tcpResult,
                                 const rsp::GUID& socketId,
                                 const std::string& hostPort,
                                 bool asyncData, bool shareSocket);

    rsp::proto::RSPMessage makeStreamReplyMessage(const rsp::proto::RSPMessage& request,
                                                   rsp::proto::STREAM_STATUS status,
                                                   const std::string& errorMessage = std::string(),
                                                   const rsp::GUID* socketId = nullptr) const;
    rsp::proto::RSPMessage makeStreamReplyMessage(const rsp::proto::NodeId& destinationNodeId,
                                                   rsp::proto::STREAM_STATUS status,
                                                   const std::string& errorMessage = std::string(),
                                                   const rsp::GUID* socketId = nullptr,
                                                   bool traceEnabled = false) const;

    static rsp::proto::StreamID toProtoStreamId(const rsp::GUID& socketId);
    static std::optional<rsp::GUID> fromProtoStreamId(const rsp::proto::StreamID& socketId);

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;

private:
    struct ManagedSocketState {
        rsp::transport::TransportHandle transport;
        rsp::transport::ConnectionHandle connection;
        rsp::proto::NodeId requesterNodeId;
        rsp::GUID socketId;
        std::string remoteEndpoint;
        bool asyncData = false;
        bool shareSocket = false;
        bool traceEnabled = false;
        std::atomic<bool> stopping = false;
        std::thread readThread;
    };

    struct ManagedListenerState {
        rsp::transport::ListeningTransportHandle transport;
        rsp::proto::NodeId requesterNodeId;
        rsp::GUID socketId;
        std::string bindEndpoint;
        bool asyncAccept = false;
        bool shareListeningSocket = false;
        bool shareChildSockets = false;
        bool childrenUseSocket = false;
        bool childrenAsyncData = false;
        bool traceEnabled = false;
        std::atomic<bool> stopping = false;
        std::mutex acceptedMutex;
        std::condition_variable acceptedChanged;
        std::deque<rsp::transport::ConnectionHandle> acceptedConnections;
    };

    bool handleConnectTCPRequest(const rsp::proto::RSPMessage& message);
    bool handleAcceptTCP(const rsp::proto::RSPMessage& message);
    bool handleStreamSend(const rsp::proto::RSPMessage& message);
    bool handleStreamRecv(const rsp::proto::RSPMessage& message);
    bool handleStreamClose(const rsp::proto::RSPMessage& message);
    void runAsyncReadLoop(const std::shared_ptr<ManagedSocketState>& socketState);
    void handleAcceptedConnection(const std::shared_ptr<ManagedListenerState>& listenerState,
                                 const rsp::transport::ConnectionHandle& connection);
    bool validateSocketAccess(const rsp::proto::RSPMessage& message,
                              const std::shared_ptr<ManagedSocketState>& socketState) const;
    bool validateListeningSocketAccess(const rsp::proto::RSPMessage& message,
                                      const std::shared_ptr<ManagedListenerState>& listenerState) const;

    void stopManagedSocket(const std::shared_ptr<ManagedSocketState>& socketState);
    void stopManagedListener(const std::shared_ptr<ManagedListenerState>& listenerState);
    void closeAllManagedSockets();
    bool publishLogPayload(const google::protobuf::Message& payload);

    mutable std::mutex socketsMutex_;
    std::map<rsp::GUID, std::shared_ptr<ManagedSocketState>> managedSockets_;
    std::map<rsp::GUID, std::shared_ptr<ManagedListenerState>> managedListeningSockets_;
    std::shared_ptr<rsp::RSPMessageQueue> connectQueue_;
    rsp::resource_manager::SchemaSnapshot loggingSchemaSnapshot_;
};

}  // namespace rsp::resource_service
