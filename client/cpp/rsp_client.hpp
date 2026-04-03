#pragma once

#include "client/cpp/rsp_client_export.hpp"
#include "client/cpp/rsp_client_message.hpp"
#include "common/message_queue.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rsp::client {

class ClientApiIncomingMessageQueue;

class RSPClient : public std::enable_shared_from_this<RSPClient> {
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

    RSPCLIENT_API int run() const;

    RSPCLIENT_API ClientConnectionID connectToResourceManager(const std::string& transport, const std::string& encoding);
    RSPCLIENT_API bool hasConnections() const;
    RSPCLIENT_API bool hasConnection(ClientConnectionID connectionId) const;
    RSPCLIENT_API std::size_t connectionCount() const;
    RSPCLIENT_API std::vector<ClientConnectionID> connectionIds() const;
    RSPCLIENT_API bool removeConnection(ClientConnectionID connectionId);
    RSPCLIENT_API bool ping(rsp::NodeID nodeId);

private:
    friend class ClientApiIncomingMessageQueue;

    struct PendingPingState {
        rsp::NodeID destination;
        uint32_t sequence = 0;
        bool completed = false;
    };

    explicit RSPClient(KeyPair keyPair);

    void receiveLoop();
    void dispatchIncomingMessage(rsp::proto::RSPMessage message);
    bool shouldHandleLocally(const rsp::proto::RSPMessage& message) const;
    void handlePingRequest(const rsp::proto::RSPMessage& message);
    void handlePingReply(const rsp::proto::RSPMessage& message);

    RSPClientMessage::Ptr messageClient_;
    rsp::MessageQueueHandle incomingMessages_;
    std::thread receiveThread_;
    mutable std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    std::map<std::string, PendingPingState> pendingPings_;
    uint32_t nextPingSequence_ = 1;
    bool stopping_ = false;
};

}  // namespace rsp::client