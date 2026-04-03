#pragma once

#include "common/ascii_handshake.hpp"
#include "common/encoding/encoding.hpp"
#include "common/message_queue.hpp"
#include "common/node.hpp"
#include "common/transport/transport.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace rsp::resource_manager {

class ResourceManager : public rsp::RSPNode {
public:
    using ConnectionQueueHandle = rsp::MessageQueueHandleT<rsp::transport::ConnectionHandle>;
    using EncodingQueueHandle = rsp::MessageQueueHandleT<rsp::encoding::EncodingHandle>;
    using NewEncodingCallback = std::function<void(const rsp::encoding::EncodingHandle& encoding)>;

    ResourceManager();
    explicit ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports);
    ~ResourceManager() override;

    int run() const override;
    void addClientTransport(const rsp::transport::ListeningTransportHandle& transport);
    size_t clientTransportCount() const;
    void setNewEncodingCallback(NewEncodingCallback callback);
    size_t activeEncodingCount() const;
    bool sendToConnection(size_t index, const rsp::proto::RSPMessage& message) const;
    bool tryDequeueMessage(rsp::proto::RSPMessage& message) const;
    size_t pendingMessageCount() const;

    void processAcceptedConnection(rsp::transport::ConnectionHandle connection);
    void processPendingEncoding(rsp::encoding::EncodingHandle encoding);

private:
    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    void handleOutputMessage(rsp::proto::RSPMessage message) override;

    void registerTransportCallbacks();
    void registerTransportCallback(const rsp::transport::ListeningTransportHandle& transport);
    void enqueueAcceptedConnection(const rsp::transport::ConnectionHandle& connection);
    rsp::encoding::EncodingHandle createEncodingForConnection(const rsp::transport::ConnectionHandle& connection) const;

    mutable std::mutex encodingsMutex_;
    std::vector<rsp::encoding::EncodingHandle> activeEncodings_;
    mutable std::mutex newEncodingCallbackMutex_;
    NewEncodingCallback newEncodingCallback_;
    rsp::MessageQueueHandle incomingMessages_;
    ConnectionQueueHandle pendingConnections_;
    EncodingQueueHandle pendingEncodings_;
    std::vector<rsp::transport::ListeningTransportHandle> clientTransports_;
};

}  // namespace rsp::resource_manager