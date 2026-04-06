#pragma once

#include "client/cpp_full/rsp_client.hpp"
#include "os/os_socket.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace rsp::resource_service {

class ResourceService : public rsp::client::full::RSPClient {
public:
    using Ptr = std::shared_ptr<ResourceService>;
    using ConstPtr = std::shared_ptr<const ResourceService>;

    static Ptr create();
    static Ptr create(KeyPair keyPair);

    ~ResourceService() override;

    ResourceService(const ResourceService&) = delete;
    ResourceService& operator=(const ResourceService&) = delete;
    ResourceService(ResourceService&&) = delete;
    ResourceService& operator=(ResourceService&&) = delete;

    ClientConnectionID connectToResourceManager(const std::string& transportSpec, const std::string& encoding);

private:
    struct ManagedSocketState {
        rsp::transport::TransportHandle transport;
        rsp::transport::ConnectionHandle connection;
        rsp::proto::NodeId requesterNodeId;
        rsp::GUID socketId;
        bool asyncData = false;
        bool shareSocket = false;
        std::atomic<bool> stopping = false;
        std::thread readThread;
    };

    struct ManagedListenerState {
        rsp::transport::ListeningTransportHandle transport;
        rsp::proto::NodeId requesterNodeId;
        rsp::GUID socketId;
        bool asyncAccept = false;
        bool shareListeningSocket = false;
        bool shareChildSockets = false;
        bool childrenUseSocket = false;
        bool childrenAsyncData = false;
        std::atomic<bool> stopping = false;
        std::mutex acceptedMutex;
        std::condition_variable acceptedChanged;
        std::deque<rsp::transport::ConnectionHandle> acceptedConnections;
    };

    explicit ResourceService(KeyPair keyPair);

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;

    bool handleConnectTCPRequest(const rsp::proto::RSPMessage& message);
    bool handleListenTCPRequest(const rsp::proto::RSPMessage& message);
    bool handleAcceptTCP(const rsp::proto::RSPMessage& message);
    bool handleSocketSend(const rsp::proto::RSPMessage& message);
    bool handleSocketRecv(const rsp::proto::RSPMessage& message);
    bool handleSocketClose(const rsp::proto::RSPMessage& message);
    void runAsyncReadLoop(const std::shared_ptr<ManagedSocketState>& socketState);
    void handleAcceptedConnection(const std::shared_ptr<ManagedListenerState>& listenerState,
                                 const rsp::transport::ConnectionHandle& connection);
    bool validateSocketAccess(const rsp::proto::RSPMessage& message,
                              const std::shared_ptr<ManagedSocketState>& socketState) const;
    bool validateListeningSocketAccess(const rsp::proto::RSPMessage& message,
                                      const std::shared_ptr<ManagedListenerState>& listenerState) const;

    rsp::proto::RSPMessage makeSocketReplyMessage(const rsp::proto::RSPMessage& request,
                                                  rsp::proto::SOCKET_STATUS status,
                                                  const std::string& errorMessage = std::string(),
                                                  const rsp::GUID* socketId = nullptr) const;
    rsp::proto::RSPMessage makeSocketReplyMessage(const rsp::proto::NodeId& destinationNodeId,
                                                  rsp::proto::SOCKET_STATUS status,
                                                  const std::string& errorMessage = std::string(),
                                                  const rsp::GUID* socketId = nullptr) const;
    void stopManagedSocket(const std::shared_ptr<ManagedSocketState>& socketState);
    void stopManagedListener(const std::shared_ptr<ManagedListenerState>& listenerState);
    void closeAllManagedSockets();

    static rsp::proto::SocketID toProtoSocketId(const rsp::GUID& socketId);
    static std::optional<rsp::GUID> fromProtoSocketId(const rsp::proto::SocketID& socketId);
    static void fillProtoAddress(const rsp::os::IPAddress& address, rsp::proto::Address* protoAddress);

    rsp::proto::ResourceAdvertisement buildResourceAdvertisement() const;
    bool sendResourceAdvertisement(ClientConnectionID connectionId) const;

    mutable std::mutex socketsMutex_;
    std::map<rsp::GUID, std::shared_ptr<ManagedSocketState>> managedSockets_;
    std::map<rsp::GUID, std::shared_ptr<ManagedListenerState>> managedListeningSockets_;
};

}  // namespace rsp::resource_service