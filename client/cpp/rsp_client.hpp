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

class RSPClient : public rsp::RSPNode, public std::enable_shared_from_this<RSPClient> {
public:
    using Ptr = std::shared_ptr<RSPClient>;
    using ConstPtr = std::shared_ptr<const RSPClient>;
    using ClientConnectionID = rsp::GUID;

    RSPCLIENT_API static Ptr create();
    RSPCLIENT_API static Ptr create(KeyPair keyPair);

    RSPCLIENT_API ~RSPClient() override = default;

    RSPClient(const RSPClient&) = delete;
    RSPClient& operator=(const RSPClient&) = delete;
    RSPClient(RSPClient&&) = delete;
    RSPClient& operator=(RSPClient&&) = delete;

    RSPCLIENT_API int run() const override;

    RSPCLIENT_API ClientConnectionID connectToResourceManager(const std::string& transport, const std::string& encoding);
    RSPCLIENT_API bool send(const rsp::proto::RSPMessage& message) const;
    RSPCLIENT_API bool sendOnConnection(ClientConnectionID connectionId, const rsp::proto::RSPMessage& message) const;
    RSPCLIENT_API bool tryDequeueMessage(rsp::proto::RSPMessage& message) const;
    RSPCLIENT_API std::size_t pendingMessageCount() const;
    RSPCLIENT_API std::optional<rsp::NodeID> peerNodeID(ClientConnectionID connectionId) const;

    RSPCLIENT_API bool hasConnections() const;
    RSPCLIENT_API bool hasConnection(ClientConnectionID connectionId) const;
    RSPCLIENT_API std::size_t connectionCount() const;
    RSPCLIENT_API std::vector<ClientConnectionID> connectionIds() const;
    RSPCLIENT_API bool removeConnection(ClientConnectionID connectionId);

private:
    struct ClientConnectionState {
        rsp::transport::TransportHandle transport;
        rsp::encoding::EncodingHandle encoding;
    };

    explicit RSPClient(KeyPair keyPair);

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