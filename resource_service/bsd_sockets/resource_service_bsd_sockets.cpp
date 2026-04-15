#include "resource_service/bsd_sockets/resource_service_bsd_sockets.hpp"

#include "common/message_queue/mq_signing.hpp"
#include "common/service_message.hpp"
#include "common/transport/transport_tcp.hpp"

#include "resource_service/bsd_sockets/bsd_sockets.pb.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace rsp::resource_service {

namespace {

std::string readBufferToString(const rsp::Buffer& buffer, uint32_t length) {
    return std::string(reinterpret_cast<const char*>(buffer.data()), length);
}

class ConnectTCPQueue : public rsp::RSPMessageQueue {
public:
    using Handler = std::function<bool(const rsp::proto::RSPMessage&)>;
    explicit ConnectTCPQueue(Handler handler) : handler_(std::move(handler)) {}

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override {
        if (handler_) {
            handler_(message);
        }
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
        std::cerr << "ResourceService TCP connect queue full — request dropped" << std::endl;
    }

private:
    Handler handler_;
};

}  // namespace

BsdSocketsResourceService::Ptr BsdSocketsResourceService::create() {
    return Ptr(new BsdSocketsResourceService(KeyPair::generateP256()));
}

BsdSocketsResourceService::Ptr BsdSocketsResourceService::create(KeyPair keyPair) {
    return Ptr(new BsdSocketsResourceService(std::move(keyPair)));
}

BsdSocketsResourceService::BsdSocketsResourceService(KeyPair keyPair)
    : ResourceService(std::move(keyPair)),
      connectQueue_(std::make_shared<ConnectTCPQueue>(
          [this](const rsp::proto::RSPMessage& message) { return handleConnectTCPRequest(message); })) {
    connectQueue_->setWorkerCount(8);
    connectQueue_->start();
}

BsdSocketsResourceService::~BsdSocketsResourceService() {
    if (connectQueue_) {
        connectQueue_->stop();
    }
    closeAllManagedSockets();
}

rsp::proto::ResourceAdvertisement BsdSocketsResourceService::buildResourceAdvertisement() const {
    rsp::proto::ResourceAdvertisement advertisement;
    const auto addresses = rsp::os::listNonLocalAddresses();

    auto* connectRecord = advertisement.add_records();
    auto* tcpConnect = connectRecord->mutable_tcp_connect();
    for (const auto& address : addresses) {
        fillProtoAddress(address, tcpConnect->add_source_addresses());
    }

    auto* listenRecord = advertisement.add_records();
    auto* tcpListen = listenRecord->mutable_tcp_listen();
    for (const auto& address : addresses) {
        fillProtoAddress(address, tcpListen->add_listen_address());
    }
    tcpListen->mutable_allowed_range()->set_start_port(0);
    tcpListen->mutable_allowed_range()->set_end_port(0);

    return advertisement;
}

bool BsdSocketsResourceService::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (rsp::hasServiceMessage<rsp::proto::ConnectTCPRequest>(message)) {
        return connectQueue_ != nullptr && connectQueue_->push(message);
    }

    if (rsp::hasServiceMessage<rsp::proto::ListenTCPRequest>(message)) {
        return handleListenTCPRequest(message);
    }

    if (rsp::hasServiceMessage<rsp::proto::AcceptTCP>(message)) {
        return handleAcceptTCP(message);
    }

    if (rsp::hasServiceMessage<rsp::proto::StreamSend>(message)) {
        return handleStreamSend(message);
    }

    if (rsp::hasServiceMessage<rsp::proto::StreamRecv>(message)) {
        return handleStreamRecv(message);
    }

    if (rsp::hasServiceMessage<rsp::proto::StreamClose>(message)) {
        return handleStreamClose(message);
    }

    return false;
}

BsdSocketsResourceService::TCPConnectionResult BsdSocketsResourceService::createTCPConnection(
    const std::string& hostPort,
    uint32_t totalAttempts,
    uint32_t retryDelayMs) {
    for (uint32_t attempt = 0; attempt < totalAttempts; ++attempt) {
        auto tcpTransport = std::make_shared<rsp::transport::TcpTransport>();
        auto connection = tcpTransport->connect(hostPort);
        if (connection != nullptr) {
            return {tcpTransport, connection};
        }

        tcpTransport->stop();
        if (attempt + 1u < totalAttempts && retryDelayMs > 0u) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
    }
    return {};
}

bool BsdSocketsResourceService::handleConnectTCPRequest(const rsp::proto::RSPMessage& message) {
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
    if (!requesterNodeId.has_value()) {
        return false;
    }

    rsp::proto::ConnectTCPRequest request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    if (!request.has_stream_id()) {
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS, "socket_number is required"));
    }

    const auto socketId = fromProtoStreamId(request.stream_id());
    if (!socketId.has_value()) {
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS, "invalid socket_number"));
    }

    if (request.host_port().empty()) {
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR, "host_port is required", &*socketId));
    }

    if (request.has_share_socket() && request.share_socket()) {
        if (request.has_async_data() && request.async_data()) {
            return send(makeStreamReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_socket cannot be combined with async_data",
                                               &*socketId));
        }

        if (request.has_use_socket() && request.use_socket()) {
            return send(makeStreamReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_socket cannot be combined with use_socket",
                                               &*socketId));
        }
    }

    const uint32_t totalAttempts = request.has_retries() ? std::min(request.retries(), 5u) + 1u : 1u;
    const uint32_t retryDelayMilliseconds = request.has_retry_ms() ? std::min(request.retry_ms(), 5000u) : 0u;

    auto tcpResult = createTCPConnection(request.host_port(), totalAttempts, retryDelayMilliseconds);

    if (tcpResult.connection == nullptr) {
        return send(makeStreamReplyMessage(message, rsp::proto::CONNECT_REFUSED, "tcp connect failed", &*socketId));
    }

    return registerConnectedSocket(message, std::move(tcpResult), *socketId,
                                   request.has_async_data() && request.async_data(),
                                   request.has_share_socket() && request.share_socket());
}

bool BsdSocketsResourceService::registerConnectedSocket(
    const rsp::proto::RSPMessage& message,
    TCPConnectionResult&& tcpResult,
    const rsp::GUID& socketId,
    bool asyncData, bool shareSocket) {

    const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
    if (!requesterNodeId.has_value()) {
        return false;
    }

    auto socketState = std::make_shared<ManagedSocketState>();
    socketState->transport = tcpResult.transport;
    socketState->connection = tcpResult.connection;
    socketState->requesterNodeId = toProtoNodeId(*requesterNodeId);
    socketState->socketId = socketId;
    socketState->asyncData = asyncData;
    socketState->shareSocket = shareSocket;
    socketState->traceEnabled = rsp::messageTraceEnabled(message);
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedSockets_.emplace(socketId, socketState);
    }

    const bool sent = send(makeStreamReplyMessage(message, rsp::proto::SUCCESS, std::string(), &socketId));
    if (!sent) {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedSockets_.erase(socketId);
        stopManagedSocket(socketState);
        return false;
    }

    if (socketState->asyncData) {
        socketState->readThread = std::thread([this, socketState]() {
            runAsyncReadLoop(socketState);
        });
    }

    return true;
}

bool BsdSocketsResourceService::handleListenTCPRequest(const rsp::proto::RSPMessage& message) {
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
    if (!requesterNodeId.has_value()) {
        return false;
    }

    rsp::proto::ListenTCPRequest request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    if (!request.has_stream_id()) {
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS, "socket_number is required"));
    }

    const auto socketId = fromProtoStreamId(request.stream_id());
    if (!socketId.has_value()) {
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS, "invalid socket_number"));
    }

    if (request.host_port().empty()) {
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR, "host_port is required", &*socketId));
    }

    const bool asyncAccept = request.has_async_accept() && request.async_accept();
    if (!asyncAccept) {
        if (request.has_share_child_sockets() && request.share_child_sockets()) {
            return send(makeStreamReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_child_sockets requires async_accept",
                                               &*socketId));
        }

        if (request.has_children_use_socket() && request.children_use_socket()) {
            return send(makeStreamReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "children_use_socket requires async_accept",
                                               &*socketId));
        }

        if (request.has_children_async_data() && request.children_async_data()) {
            return send(makeStreamReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "children_async_data requires async_accept",
                                               &*socketId));
        }
    }

    auto tcpTransport = std::make_shared<rsp::transport::TcpTransport>();
    auto listenerState = std::make_shared<ManagedListenerState>();
    listenerState->transport = tcpTransport;
    listenerState->requesterNodeId = toProtoNodeId(*requesterNodeId);
    listenerState->socketId = *socketId;
    listenerState->asyncAccept = asyncAccept;
    listenerState->shareListeningSocket = request.has_share_listening_socket() && request.share_listening_socket();
    listenerState->shareChildSockets = request.has_share_child_sockets() && request.share_child_sockets();
    listenerState->childrenUseSocket = request.has_children_use_socket() && request.children_use_socket();
    listenerState->childrenAsyncData = request.has_children_async_data() && request.children_async_data();
    listenerState->traceEnabled = rsp::messageTraceEnabled(message);
    tcpTransport->setNewConnectionCallback([this, listenerState](const rsp::transport::ConnectionHandle& connection) {
        handleAcceptedConnection(listenerState, connection);
    });

    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        if (managedSockets_.find(*socketId) != managedSockets_.end() ||
            managedListeningSockets_.find(*socketId) != managedListeningSockets_.end()) {
            return send(makeStreamReplyMessage(message, rsp::proto::STREAM_IN_USE, "socket id already exists", &*socketId));
        }
    }

    if (!tcpTransport->listen(request.host_port())) {
        tcpTransport->stop();
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR, "tcp listen failed", &*socketId));
    }

    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedListeningSockets_.emplace(*socketId, listenerState);
    }

    const bool sent = send(makeStreamReplyMessage(message, rsp::proto::SUCCESS, std::string(), &*socketId));
    if (!sent) {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedListeningSockets_.erase(*socketId);
        stopManagedListener(listenerState);
        return false;
    }

    return true;
}

bool BsdSocketsResourceService::handleAcceptTCP(const rsp::proto::RSPMessage& message) {
    rsp::proto::AcceptTCP request;
    if (!rsp::unpackServiceMessage(message, &request)) {
        return false;
    }

    const auto listenSocketId = fromProtoStreamId(request.listen_stream_id());
    if (!listenSocketId.has_value()) {
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR, "invalid listen socket id"));
    }

    std::shared_ptr<ManagedListenerState> listenerState;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        const auto iterator = managedListeningSockets_.find(*listenSocketId);
        if (iterator == managedListeningSockets_.end()) {
            return send(makeStreamReplyMessage(message, rsp::proto::STREAM_CLOSED, "listen socket not found", &*listenSocketId));
        }

        listenerState = iterator->second;
    }

    if (!validateListeningSocketAccess(message, listenerState)) {
        return send(makeStreamReplyMessage(message,
                                           rsp::proto::NODEID_MISMATCH,
                                           "listen socket is owned by a different node id",
                                           &*listenSocketId));
    }

    if (listenerState->asyncAccept) {
        return send(makeStreamReplyMessage(message,
                                           rsp::proto::ASYNC_STREAM,
                                           "accept_tcp is ignored when async_accept is enabled",
                                           &*listenSocketId));
    }

    rsp::transport::ConnectionHandle acceptedConnection;
    {
        std::unique_lock<std::mutex> lock(listenerState->acceptedMutex);
        const auto timeout = request.has_timeout_ms() ? request.timeout_ms() : 0u;
        if (listenerState->acceptedConnections.empty()) {
            const auto ready = timeout > 0
                                   ? listenerState->acceptedChanged.wait_for(lock,
                                                                             std::chrono::milliseconds(timeout),
                                                                             [listenerState]() {
                                                                                 return listenerState->stopping.load() ||
                                                                                        !listenerState->acceptedConnections.empty();
                                                                             })
                                   : listenerState->acceptedChanged.wait_for(lock,
                                                                             std::chrono::seconds(5),
                                                                             [listenerState]() {
                                                                                 return listenerState->stopping.load() ||
                                                                                        !listenerState->acceptedConnections.empty();
                                                                             });
            if (!ready) {
                return send(makeStreamReplyMessage(message, rsp::proto::TIMED_OUT, "accept timed out", &*listenSocketId));
            }
        }

        if (listenerState->stopping.load() || listenerState->acceptedConnections.empty()) {
            return send(makeStreamReplyMessage(message, rsp::proto::STREAM_CLOSED, "listen socket closed", &*listenSocketId));
        }

        acceptedConnection = listenerState->acceptedConnections.front();
        listenerState->acceptedConnections.pop_front();
    }

    const auto childSocketId = request.has_new_stream_id()
                                   ? fromProtoStreamId(request.new_stream_id())
                                   : std::optional<rsp::GUID>(rsp::GUID());
    if (!childSocketId.has_value()) {
        if (acceptedConnection != nullptr) {
            acceptedConnection->close();
        }
        return send(makeStreamReplyMessage(message, rsp::proto::INVALID_FLAGS, "invalid new_socket_number", &*listenSocketId));
    }

    auto socketState = std::make_shared<ManagedSocketState>();
    socketState->connection = acceptedConnection;
    socketState->requesterNodeId = toProtoNodeId(*rsp::senderNodeIdFromMessage(message));
    socketState->socketId = *childSocketId;
    socketState->asyncData = request.has_child_async_data() && request.child_async_data();
    socketState->shareSocket = request.has_share_child_socket() && request.share_child_socket();
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        if (managedSockets_.find(*childSocketId) != managedSockets_.end() ||
            managedListeningSockets_.find(*childSocketId) != managedListeningSockets_.end()) {
            if (socketState->connection != nullptr) {
                socketState->connection->close();
            }
            return send(makeStreamReplyMessage(message, rsp::proto::STREAM_IN_USE, "socket id already exists", &*listenSocketId));
        }

        managedSockets_.emplace(*childSocketId, socketState);
    }

    auto reply = makeStreamReplyMessage(message, rsp::proto::SUCCESS, std::string(), &*listenSocketId);
    // Unpack, add child socket id, repack
    rsp::proto::StreamReply socketReply;
    rsp::unpackServiceMessage(reply, &socketReply);
    *socketReply.mutable_new_stream_id() = toProtoStreamId(*childSocketId);
    rsp::packServiceMessage(reply, socketReply);
    const bool sent = send(reply);
    if (!sent) {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedSockets_.erase(*childSocketId);
        stopManagedSocket(socketState);
        return false;
    }

    if (socketState->asyncData) {
        socketState->readThread = std::thread([this, socketState]() {
            runAsyncReadLoop(socketState);
        });
    }

    return true;
}

bool BsdSocketsResourceService::handleStreamSend(const rsp::proto::RSPMessage& message) {
    const bool trace = rsp::messageTraceEnabled(message);

    rsp::proto::StreamSend sendMsg;
    if (!rsp::unpackServiceMessage(message, &sendMsg)) {
        return false;
    }

    const auto socketId = fromProtoStreamId(sendMsg.stream_id());
    if (!socketId.has_value()) {
        if (trace) {
            std::cerr << "[resource_service] socket_send invalid socket id" << std::endl;
        }
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR, "invalid socket id"));
    }

    std::shared_ptr<ManagedSocketState> socketState;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        const auto iterator = managedSockets_.find(*socketId);
        if (iterator == managedSockets_.end()) {
            if (trace) {
                std::cerr << "[resource_service] socket_send socket not found id=" << socketId->toString() << std::endl;
            }
            return send(makeStreamReplyMessage(message, rsp::proto::STREAM_CLOSED, "socket not found"));
        }

        socketState = iterator->second;
    }

    if (!validateSocketAccess(message, socketState)) {
        if (trace || (socketState != nullptr && socketState->traceEnabled)) {
            std::cerr << "[resource_service] socket_send node mismatch id=" << socketId->toString() << std::endl;
        }
        return send(makeStreamReplyMessage(message, rsp::proto::NODEID_MISMATCH,
                                           "socket is owned by a different node id"));
    }

    const bool effectiveTrace = trace || (socketState != nullptr && socketState->traceEnabled);

    const std::string& data = sendMsg.data();
    if (effectiveTrace) {
        std::cerr << "[resource_service] socket_send id=" << socketId->toString()
                  << " bytes=" << data.size() << std::endl;
    }
    const bool sent = socketState != nullptr && socketState->connection != nullptr &&
                      socketState->connection->sendAll(reinterpret_cast<const uint8_t*>(data.data()),
                                                      static_cast<uint32_t>(data.size()));
    if (!sent) {
        if (effectiveTrace) {
            std::cerr << "[resource_service] socket_send sendAll failed id=" << socketId->toString() << std::endl;
        }
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR, "socket send failed"));
    }

    if (effectiveTrace) {
        std::cerr << "[resource_service] socket_send success id=" << socketId->toString() << std::endl;
    }
    return send(makeStreamReplyMessage(message, rsp::proto::SUCCESS));
}

bool BsdSocketsResourceService::handleStreamRecv(const rsp::proto::RSPMessage& message) {
    rsp::proto::StreamRecv recvMsg;
    if (!rsp::unpackServiceMessage(message, &recvMsg)) {
        return false;
    }

    const auto socketId = fromProtoStreamId(recvMsg.stream_id());
    if (!socketId.has_value()) {
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR, "invalid socket id"));
    }

    std::shared_ptr<ManagedSocketState> socketState;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        const auto iterator = managedSockets_.find(*socketId);
        if (iterator == managedSockets_.end()) {
            return send(makeStreamReplyMessage(message, rsp::proto::STREAM_CLOSED, "socket not found"));
        }

        socketState = iterator->second;
    }

    if (!validateSocketAccess(message, socketState)) {
        return send(makeStreamReplyMessage(message, rsp::proto::NODEID_MISMATCH,
                                           "socket is owned by a different node id"));
    }

    if (socketState != nullptr && socketState->asyncData) {
        return send(makeStreamReplyMessage(message, rsp::proto::ASYNC_STREAM,
                                           "socket_recv is ignored when async_data is enabled"));
    }

    const uint32_t maxBytes = recvMsg.has_max_bytes() && recvMsg.max_bytes() > 0
                                  ? recvMsg.max_bytes()
                                  : 4096u;
    rsp::Buffer buffer(maxBytes);
    const int bytesRead = socketState == nullptr || socketState->connection == nullptr ? -1 : socketState->connection->recv(buffer);
    if (bytesRead < 0) {
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR, "socket recv failed"));
    }

    if (bytesRead == 0) {
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_CLOSED, "socket closed"));
    }

    auto reply = makeStreamReplyMessage(message, rsp::proto::STREAM_DATA);
    rsp::proto::StreamReply sr;
    rsp::unpackServiceMessage(reply, &sr);
    sr.set_data(readBufferToString(buffer, static_cast<uint32_t>(bytesRead)));
    rsp::packServiceMessage(reply, sr);
    return send(reply);
}

bool BsdSocketsResourceService::handleStreamClose(const rsp::proto::RSPMessage& message) {
    rsp::proto::StreamClose closeMsg;
    if (!rsp::unpackServiceMessage(message, &closeMsg)) {
        return false;
    }

    const auto socketId = fromProtoStreamId(closeMsg.stream_id());
    if (!socketId.has_value()) {
        return send(makeStreamReplyMessage(message, rsp::proto::STREAM_ERROR, "invalid socket id"));
    }

    std::shared_ptr<ManagedListenerState> removedListener;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        const auto iterator = managedListeningSockets_.find(*socketId);
        if (iterator != managedListeningSockets_.end()) {
            removedListener = iterator->second;
            managedListeningSockets_.erase(iterator);
        }
    }

    if (removedListener != nullptr) {
        if (!validateListeningSocketAccess(message, removedListener)) {
            std::lock_guard<std::mutex> lock(socketsMutex_);
            managedListeningSockets_.emplace(*socketId, removedListener);
            return send(makeStreamReplyMessage(message,
                                               rsp::proto::NODEID_MISMATCH,
                                               "listen socket is owned by a different node id",
                                               &*socketId));
        }

        stopManagedListener(removedListener);
        return send(makeStreamReplyMessage(message, rsp::proto::SUCCESS, std::string(), &*socketId));
    }

    std::shared_ptr<ManagedSocketState> removedSocket;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        const auto iterator = managedSockets_.find(*socketId);
        if (iterator == managedSockets_.end()) {
            return send(makeStreamReplyMessage(message, rsp::proto::STREAM_CLOSED, "socket not found"));
        }

        removedSocket = iterator->second;
        managedSockets_.erase(iterator);
    }

    if (!validateSocketAccess(message, removedSocket)) {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedSockets_.emplace(*socketId, removedSocket);
        return send(makeStreamReplyMessage(message, rsp::proto::NODEID_MISMATCH,
                                           "socket is owned by a different node id"));
    }

    stopManagedSocket(removedSocket);

    return send(makeStreamReplyMessage(message, rsp::proto::SUCCESS));
}

void BsdSocketsResourceService::handleAcceptedConnection(const std::shared_ptr<ManagedListenerState>& listenerState,
                                                         const rsp::transport::ConnectionHandle& connection) {
    if (listenerState == nullptr || connection == nullptr || listenerState->stopping.load()) {
        if (connection != nullptr) {
            connection->close();
        }
        return;
    }

    if (!listenerState->asyncAccept) {
        std::lock_guard<std::mutex> lock(listenerState->acceptedMutex);
        listenerState->acceptedConnections.push_back(connection);
        listenerState->acceptedChanged.notify_all();
        return;
    }

    auto socketState = std::make_shared<ManagedSocketState>();
    socketState->connection = connection;
    socketState->requesterNodeId = listenerState->requesterNodeId;
    socketState->socketId = rsp::GUID();
    socketState->asyncData = listenerState->childrenAsyncData;
    socketState->shareSocket = listenerState->shareChildSockets;
    socketState->traceEnabled = listenerState->traceEnabled;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        while (managedSockets_.find(socketState->socketId) != managedSockets_.end() ||
               managedListeningSockets_.find(socketState->socketId) != managedListeningSockets_.end()) {
            socketState->socketId = rsp::GUID();
        }
        managedSockets_.emplace(socketState->socketId, socketState);
    }

    auto reply = makeStreamReplyMessage(listenerState->requesterNodeId,
                                        rsp::proto::NEW_CONNECTION,
                                        std::string(),
                                        &listenerState->socketId,
                                        listenerState->traceEnabled);
    {
        rsp::proto::StreamReply sr;
        rsp::unpackServiceMessage(reply, &sr);
        *sr.mutable_new_stream_id() = toProtoStreamId(socketState->socketId);
        rsp::packServiceMessage(reply, sr);
    }
    if (!send(reply)) {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedSockets_.erase(socketState->socketId);
        stopManagedSocket(socketState);
        return;
    }

    if (socketState->asyncData) {
        socketState->readThread = std::thread([this, socketState]() {
            runAsyncReadLoop(socketState);
        });
    }
}

void BsdSocketsResourceService::runAsyncReadLoop(const std::shared_ptr<ManagedSocketState>& socketState) {
    if (socketState == nullptr || socketState->connection == nullptr) {
        return;
    }

    if (socketState->traceEnabled) {
        std::cerr << "[resource_service] async_read_loop start id=" << socketState->socketId.toString() << std::endl;
    }

    while (!socketState->stopping.load()) {
        rsp::Buffer buffer(4096);
        const int bytesRead = socketState->connection->recv(buffer);
        if (socketState->traceEnabled) {
            std::cerr << "[resource_service] async_read_loop recv id=" << socketState->socketId.toString()
                      << " bytes=" << bytesRead << std::endl;
        }
        if (bytesRead < 0) {
            if (!socketState->stopping.load()) {
                const auto reply = makeStreamReplyMessage(socketState->requesterNodeId,
                                                          rsp::proto::STREAM_ERROR,
                                                          "socket recv failed",
                                                          &socketState->socketId,
                                                          socketState->traceEnabled);
                if (socketState->traceEnabled) {
                    std::cerr << "[resource_service] async_read_loop send STREAM_ERROR id="
                              << socketState->socketId.toString() << std::endl;
                }
                send(reply);
            }
            break;
        }

        if (bytesRead == 0) {
            if (!socketState->stopping.load()) {
                const auto reply = makeStreamReplyMessage(socketState->requesterNodeId,
                                                          rsp::proto::STREAM_CLOSED,
                                                          "socket closed",
                                                          &socketState->socketId,
                                                          socketState->traceEnabled);
                if (socketState->traceEnabled) {
                    std::cerr << "[resource_service] async_read_loop send STREAM_CLOSED id="
                              << socketState->socketId.toString() << std::endl;
                }
                send(reply);
            }
            break;
        }

        auto reply = makeStreamReplyMessage(socketState->requesterNodeId,
                                            rsp::proto::STREAM_DATA,
                                            std::string(),
                                            &socketState->socketId,
                                            socketState->traceEnabled);
        {
            rsp::proto::StreamReply sr;
            rsp::unpackServiceMessage(reply, &sr);
            sr.set_data(readBufferToString(buffer, static_cast<uint32_t>(bytesRead)));
            rsp::packServiceMessage(reply, sr);
        }
        if (socketState->traceEnabled) {
            std::cerr << "[resource_service] async_read_loop send STREAM_DATA id="
                      << socketState->socketId.toString() << " bytes=" << bytesRead << std::endl;
        }
        if (!send(reply)) {
            break;
        }
    }

    if (socketState->traceEnabled) {
        std::cerr << "[resource_service] async_read_loop end id=" << socketState->socketId.toString() << std::endl;
    }
}

bool BsdSocketsResourceService::validateSocketAccess(const rsp::proto::RSPMessage& message,
                                                     const std::shared_ptr<ManagedSocketState>& socketState) const {
    if (socketState == nullptr || socketState->shareSocket) {
        return true;
    }

    const auto senderNodeId = rsp::senderNodeIdFromMessage(message);
    if (!senderNodeId.has_value()) {
        return false;
    }

    return toProtoNodeId(*senderNodeId).value() == socketState->requesterNodeId.value();
}

bool BsdSocketsResourceService::validateListeningSocketAccess(const rsp::proto::RSPMessage& message,
                                                              const std::shared_ptr<ManagedListenerState>& listenerState) const {
    if (listenerState == nullptr || listenerState->shareListeningSocket) {
        return true;
    }

    const auto senderNodeId = rsp::senderNodeIdFromMessage(message);
    if (!senderNodeId.has_value()) {
        return false;
    }

    return toProtoNodeId(*senderNodeId).value() == listenerState->requesterNodeId.value();
}

rsp::proto::RSPMessage BsdSocketsResourceService::makeStreamReplyMessage(const rsp::proto::RSPMessage& request,
                                                                         rsp::proto::STREAM_STATUS status,
                                                                         const std::string& errorMessage,
                                                                         const rsp::GUID* socketId) const {
    const rsp::GUID* effectiveSocketId = socketId;
    std::optional<rsp::GUID> derivedSocketId;
    if (effectiveSocketId == nullptr) {
        rsp::proto::ConnectTCPRequest connectReq;
        rsp::proto::ListenTCPRequest listenReq;
        rsp::proto::AcceptTCP acceptReq;
        rsp::proto::StreamSend sendReq;
        rsp::proto::StreamRecv recvReq;
        rsp::proto::StreamClose closeReq;

        if (rsp::unpackServiceMessage(request, &connectReq) && connectReq.has_stream_id()) {
            derivedSocketId = fromProtoStreamId(connectReq.stream_id());
        } else if (rsp::unpackServiceMessage(request, &listenReq) && listenReq.has_stream_id()) {
            derivedSocketId = fromProtoStreamId(listenReq.stream_id());
        } else if (rsp::unpackServiceMessage(request, &acceptReq)) {
            derivedSocketId = fromProtoStreamId(acceptReq.listen_stream_id());
        } else if (rsp::unpackServiceMessage(request, &sendReq)) {
            derivedSocketId = fromProtoStreamId(sendReq.stream_id());
        } else if (rsp::unpackServiceMessage(request, &recvReq)) {
            derivedSocketId = fromProtoStreamId(recvReq.stream_id());
        } else if (rsp::unpackServiceMessage(request, &closeReq)) {
            derivedSocketId = fromProtoStreamId(closeReq.stream_id());
        }

        if (derivedSocketId.has_value()) {
            effectiveSocketId = &*derivedSocketId;
        }
    }

    const auto senderNodeId = rsp::senderNodeIdFromMessage(request);
    if (!senderNodeId.has_value()) {
        return rsp::proto::RSPMessage();
    }

    auto reply = makeStreamReplyMessage(toProtoNodeId(*senderNodeId), status, errorMessage, effectiveSocketId);
    rsp::copyMessageTrace(request, reply);
    return reply;
}

rsp::proto::RSPMessage BsdSocketsResourceService::makeStreamReplyMessage(const rsp::proto::NodeId& destinationNodeId,
                                                                         rsp::proto::STREAM_STATUS status,
                                                                         const std::string& errorMessage,
                                                                         const rsp::GUID* socketId,
                                                                         bool traceEnabled) const {
    rsp::proto::RSPMessage reply;
    *reply.mutable_destination() = destinationNodeId;
    if (traceEnabled) {
        reply.mutable_trace()->set_value(true);
    }

    rsp::proto::StreamReply socketReply;
    if (socketId != nullptr) {
        *socketReply.mutable_stream_id() = toProtoStreamId(*socketId);
    }
    socketReply.set_error(status);
    if (!errorMessage.empty()) {
        socketReply.set_message(errorMessage);
    }
    if (socketId != nullptr) {
        *socketReply.mutable_new_stream_id() = toProtoStreamId(*socketId);
    }
    rsp::packServiceMessage(reply, socketReply);

    return reply;
}

void BsdSocketsResourceService::stopManagedSocket(const std::shared_ptr<ManagedSocketState>& socketState) {
    if (socketState == nullptr) {
        return;
    }

    socketState->stopping.store(true);
    if (socketState->connection != nullptr) {
        socketState->connection->close();
    }
    if (socketState->transport != nullptr) {
        socketState->transport->stop();
    }
    if (socketState->readThread.joinable()) {
        socketState->readThread.join();
    }
}

void BsdSocketsResourceService::stopManagedListener(const std::shared_ptr<ManagedListenerState>& listenerState) {
    if (listenerState == nullptr) {
        return;
    }

    listenerState->stopping.store(true);
    listenerState->acceptedChanged.notify_all();
    if (listenerState->transport != nullptr) {
        listenerState->transport->stop();
    }

    std::deque<rsp::transport::ConnectionHandle> acceptedConnections;
    {
        std::lock_guard<std::mutex> lock(listenerState->acceptedMutex);
        acceptedConnections.swap(listenerState->acceptedConnections);
    }

    for (auto& connection : acceptedConnections) {
        if (connection != nullptr) {
            connection->close();
        }
    }
}

void BsdSocketsResourceService::closeAllManagedSockets() {
    std::map<rsp::GUID, std::shared_ptr<ManagedSocketState>> removedSockets;
    std::map<rsp::GUID, std::shared_ptr<ManagedListenerState>> removedListeners;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        removedSockets.swap(managedSockets_);
        removedListeners.swap(managedListeningSockets_);
    }

    for (auto& [_, listenerState] : removedListeners) {
        stopManagedListener(listenerState);
    }

    for (auto& [_, socketState] : removedSockets) {
        stopManagedSocket(socketState);
    }
}

rsp::proto::StreamID BsdSocketsResourceService::toProtoStreamId(const rsp::GUID& socketId) {
    rsp::proto::StreamID protoSocketId;
    std::string value(16, '\0');
    const uint64_t high = socketId.high();
    const uint64_t low = socketId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoSocketId.set_value(value);
    return protoSocketId;
}

std::optional<rsp::GUID> BsdSocketsResourceService::fromProtoStreamId(const rsp::proto::StreamID& socketId) {
    if (socketId.value().size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, socketId.value().data(), sizeof(high));
    std::memcpy(&low, socketId.value().data() + sizeof(high), sizeof(low));
    return rsp::GUID(high, low);
}

}  // namespace rsp::resource_service
