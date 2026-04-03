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
#include <vector>

namespace rsp::client {

class RSPClient : public rsp::RSPNode, public std::enable_shared_from_this<RSPClient> {
public:
    using Ptr = std::shared_ptr<RSPClient>;
    using ConstPtr = std::shared_ptr<const RSPClient>;
    using TransportID = rsp::GUID;

    RSPCLIENT_API static Ptr create();
    RSPCLIENT_API static Ptr create(KeyPair keyPair);

    RSPCLIENT_API ~RSPClient() override = default;

    RSPClient(const RSPClient&) = delete;
    RSPClient& operator=(const RSPClient&) = delete;
    RSPClient(RSPClient&&) = delete;
    RSPClient& operator=(RSPClient&&) = delete;

    RSPCLIENT_API int run() const override;

    RSPCLIENT_API TransportID createTcpTransport();
    RSPCLIENT_API TransportID addTransport(const std::shared_ptr<rsp::transport::Transport>& transport);
    RSPCLIENT_API rsp::transport::ConnectionHandle connect(TransportID transportId, const std::string& parameters) const;
    RSPCLIENT_API bool send(TransportID transportId, const rsp::proto::RSPMessage& message) const;
    RSPCLIENT_API bool tryDequeueMessage(rsp::proto::RSPMessage& message) const;
    RSPCLIENT_API std::size_t pendingMessageCount() const;

    RSPCLIENT_API bool hasTransports() const;
    RSPCLIENT_API bool hasTransport(TransportID transportId) const;
    RSPCLIENT_API std::size_t transportCount() const;
    RSPCLIENT_API std::vector<TransportID> transportIds() const;
    RSPCLIENT_API bool removeTransport(TransportID transportId);

    RSPCLIENT_API rsp::transport::TransportHandle transport(TransportID transportId) const;

private:
    explicit RSPClient(KeyPair keyPair);

    bool performAsciiHandshake(const rsp::transport::ConnectionHandle& connection) const;
    rsp::encoding::EncodingHandle encoding(TransportID transportId) const;

    mutable std::mutex transportsMutex_;
    std::map<TransportID, rsp::transport::TransportHandle> transports_;
    mutable std::map<TransportID, rsp::encoding::EncodingHandle> encodings_;
    rsp::MessageQueueHandle incomingMessages_;
};

}  // namespace rsp::client