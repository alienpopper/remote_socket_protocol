#pragma once

#include "common/endorsement/endorsement.hpp"
#include "common/encoding/encoding.hpp"
#include "common/message_queue/mq.hpp"
#include "common/node.hpp"
#include "common/transport/transport.hpp"
#include "resource_manager/schema_registry.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace rsp::resource_manager {

class ResourceManager : public rsp::RSPNode {
public:
    using ConnectionQueueHandle = rsp::MessageQueueHandleT<rsp::transport::ConnectionHandle>;
    using EncodingQueueHandle = rsp::MessageQueueHandleT<rsp::encoding::EncodingHandle>;
    using NewEncodingCallback = std::function<void(const rsp::encoding::EncodingHandle& encoding)>;

    ResourceManager();
    explicit ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports);
    ResourceManager(rsp::KeyPair keyPair, std::vector<rsp::transport::ListeningTransportHandle> clientTransports);
    ~ResourceManager() override;

    int run() const override;
    void addClientTransport(const rsp::transport::ListeningTransportHandle& transport);
    size_t clientTransportCount() const;
    void setNewEncodingCallback(NewEncodingCallback callback);
    size_t activeEncodingCount() const;
    size_t resourceAdvertisementCount() const;
    bool hasResourceAdvertisement(const rsp::NodeID& nodeId) const;
    std::optional<rsp::proto::ResourceAdvertisement> resourceAdvertisement(const rsp::NodeID& nodeId) const;
    size_t schemaCount() const;
    bool sendToConnection(size_t index, const rsp::proto::RSPMessage& message) const;
    bool routeAndSend(const rsp::proto::RSPMessage& message) const;
    bool isForThisNode(const rsp::proto::RSPMessage& message) const;
    bool tryDequeueMessage(rsp::proto::RSPMessage& message) const;
    size_t pendingMessageCount() const;

protected:
    virtual std::vector<rsp::Endorsement> getAuthorizationEndorsements(const rsp::NodeID& nodeId) const;
    virtual rsp::proto::ERDAbstractSyntaxTree authorizationTree() const;
    void rebuildAuthorizationQueue();

private:
    struct ActiveEncodingState {
        rsp::encoding::EncodingHandle encoding;
        rsp::MessageQueueHandle signingQueue;
    };

    /*messages sent to this RM, not routed to other nodes*/
    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    /*messages produced by this RM, not routed through*/
    void handleOutputMessage(rsp::proto::RSPMessage message) override;
    bool sendLocalMessage(const rsp::proto::RSPMessage& message) const;

    void eraseResourceAdvertisement(const rsp::NodeID& nodeId) const;
    void registerTransportCallbacks();
    void registerTransportCallback(const rsp::transport::ListeningTransportHandle& transport);
    void enqueueAcceptedConnection(const rsp::transport::ConnectionHandle& connection);
    void handleHandshakeSuccess(const rsp::encoding::EncodingHandle& encoding);
    void handleAuthNSuccess(const rsp::encoding::EncodingHandle& encoding);
    void handleAuthNFailure(const rsp::encoding::EncodingHandle& encoding);
    void handleVerifiedMessage(rsp::proto::RSPMessage message);
    void handleSignatureFailure(rsp::proto::RSPMessage message, const std::string& reason);
    void handleAuthorizedMessage(rsp::proto::RSPMessage message);
    void handleAuthorizationFailure(rsp::proto::RSPMessage message);
    void cacheAuthenticatedIdentity(const rsp::NodeID& peerNodeId, const rsp::proto::Identity& identity);
    std::shared_ptr<const rsp::KeyPair> verificationKeyForNodeId(const rsp::NodeID& nodeId) const;
    void sendSignatureFailure(const rsp::proto::RSPMessage& rejectedMessage, const std::string& reason) const;
    void sendEndorsementNeeded(const rsp::proto::RSPMessage& rejectedMessage) const;

    mutable std::mutex encodingsMutex_;
    std::vector<ActiveEncodingState> activeEncodings_;
    mutable std::mutex resourceAdvertisementsMutex_;
    mutable std::map<rsp::NodeID, rsp::proto::ResourceAdvertisement> resourceAdvertisements_;
    SchemaRegistry schemaRegistry_;
    mutable std::mutex newEncodingCallbackMutex_;
    NewEncodingCallback newEncodingCallback_;
    mutable std::mutex authzQueueMutex_;
    rsp::MessageQueueHandle incomingMessages_;
    rsp::MessageQueueHandle authzQueue_;
    rsp::MessageQueueHandle signatureCheckQueue_;
    ConnectionQueueHandle handshakeQueue_;
    EncodingQueueHandle authnQueue_;
    std::vector<rsp::transport::ListeningTransportHandle> clientTransports_;
};

}  // namespace rsp::resource_manager