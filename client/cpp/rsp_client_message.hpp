#pragma once

#include "client/cpp/rsp_client_export.hpp"
#include "common/encoding/encoding.hpp"
#include "common/message_queue.hpp"
#include "common/node.hpp"
#include "common/transport/transport.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rsp::client {

class RSPClientMessage : public rsp::RSPNode, public std::enable_shared_from_this<RSPClientMessage> {
public:
    using Ptr = std::shared_ptr<RSPClientMessage>;
    using ConstPtr = std::shared_ptr<const RSPClientMessage>;
    using ClientConnectionID = rsp::GUID;

    RSPCLIENT_API static Ptr create();
    RSPCLIENT_API static Ptr create(KeyPair keyPair);

    RSPCLIENT_API ~RSPClientMessage() override = default;

    RSPClientMessage(const RSPClientMessage&) = delete;
    RSPClientMessage& operator=(const RSPClientMessage&) = delete;
    RSPClientMessage(RSPClientMessage&&) = delete;
    RSPClientMessage& operator=(RSPClientMessage&&) = delete;

    RSPCLIENT_API int run() const override;

    RSPCLIENT_API ClientConnectionID connectToResourceManager(const std::string& transport, const std::string& encoding);
    RSPCLIENT_API bool send(const rsp::proto::RSPMessage& message) const;
    RSPCLIENT_API bool sendOnConnection(ClientConnectionID connectionId, const rsp::proto::RSPMessage& message) const;
    RSPCLIENT_API bool hasConnections() const;
    RSPCLIENT_API bool hasConnection(ClientConnectionID connectionId) const;
    RSPCLIENT_API std::size_t connectionCount() const;
    RSPCLIENT_API std::vector<ClientConnectionID> connectionIds() const;
    RSPCLIENT_API bool removeConnection(ClientConnectionID connectionId);

    RSPCLIENT_API bool tryDequeueMessage(rsp::proto::RSPMessage& message) const;
    RSPCLIENT_API std::size_t pendingMessageCount() const;
    RSPCLIENT_API std::optional<rsp::NodeID> peerNodeID(ClientConnectionID connectionId) const;
    RSPCLIENT_API rsp::NodeID nodeId() const;

private:
    struct ClientConnectionState {
        rsp::transport::TransportHandle transport;
        rsp::encoding::EncodingHandle encoding;
    };

    explicit RSPClientMessage(KeyPair keyPair);

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    void handleOutputMessage(rsp::proto::RSPMessage message) override;
    bool performAsciiHandshake(const rsp::transport::ConnectionHandle& connection, const std::string& encoding) const;
    rsp::transport::TransportHandle createTransport(const std::string& transportName) const;
    rsp::encoding::EncodingHandle createEncoding(const rsp::transport::ConnectionHandle& connection,
                                                 const std::string& encoding) const;
    std::optional<ClientConnectionState> connectionState(ClientConnectionID connectionId) const;

    mutable std::mutex connectionsMutex_;
    std::map<ClientConnectionID, ClientConnectionState> connections_;
    rsp::MessageQueueHandle incomingMessages_;
};

}  // namespace rsp::client