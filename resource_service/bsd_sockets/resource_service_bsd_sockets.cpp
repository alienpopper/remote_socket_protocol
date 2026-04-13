#include "resource_service/bsd_sockets/resource_service_bsd_sockets.hpp"

#include "common/message_queue/mq_signing.hpp"
#include "common/transport/transport_tcp.hpp"

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
    if (message.has_connect_tcp_request()) {
        return connectQueue_ != nullptr && connectQueue_->push(message);
    }

    if (message.has_listen_tcp_request()) {
        return handleListenTCPRequest(message);
    }

    if (message.has_accept_tcp()) {
        return handleAcceptTCP(message);
    }

    if (message.has_socket_send()) {
        return handleSocketSend(message);
    }

    if (message.has_socket_recv()) {
        return handleSocketRecv(message);
    }

    if (message.has_socket_close()) {
        return handleSocketClose(message);
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

    const auto& request = message.connect_tcp_request();
    if (!request.has_socket_number()) {
        return send(makeSocketReplyMessage(message, rsp::proto::INVALID_FLAGS, "socket_number is required"));
    }

    const auto socketId = fromProtoSocketId(request.socket_number());
    if (!socketId.has_value()) {
        return send(makeSocketReplyMessage(message, rsp::proto::INVALID_FLAGS, "invalid socket_number"));
    }

    if (request.host_port().empty()) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "host_port is required", &*socketId));
    }

    if (request.has_share_socket() && request.share_socket()) {
        if (request.has_async_data() && request.async_data()) {
            return send(makeSocketReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_socket cannot be combined with async_data",
                                               &*socketId));
        }

        if (request.has_use_socket() && request.use_socket()) {
            return send(makeSocketReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_socket cannot be combined with use_socket",
                                               &*socketId));
        }
    }

    const uint32_t totalAttempts = request.has_retries() ? std::min(request.retries(), 5u) + 1u : 1u;
    const uint32_t retryDelayMilliseconds = request.has_retry_ms() ? std::min(request.retry_ms(), 5000u) : 0u;

    auto tcpResult = createTCPConnection(request.host_port(), totalAttempts, retryDelayMilliseconds);

    if (tcpResult.connection == nullptr) {
        return send(makeSocketReplyMessage(message, rsp::proto::CONNECT_REFUSED, "tcp connect failed", &*socketId));
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

    const bool sent = send(makeSocketReplyMessage(message, rsp::proto::SUCCESS, std::string(), &socketId));
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

    const auto& request = message.listen_tcp_request();
    if (!request.has_socket_number()) {
        return send(makeSocketReplyMessage(message, rsp::proto::INVALID_FLAGS, "socket_number is required"));
    }

    const auto socketId = fromProtoSocketId(request.socket_number());
    if (!socketId.has_value()) {
        return send(makeSocketReplyMessage(message, rsp::proto::INVALID_FLAGS, "invalid socket_number"));
    }

    if (request.host_port().empty()) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "host_port is required", &*socketId));
    }

    const bool asyncAccept = request.has_async_accept() && request.async_accept();
    if (!asyncAccept) {
        if (request.has_share_child_sockets() && request.share_child_sockets()) {
            return send(makeSocketReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "share_child_sockets requires async_accept",
                                               &*socketId));
        }

        if (request.has_children_use_socket() && request.children_use_socket()) {
            return send(makeSocketReplyMessage(message,
                                               rsp::proto::INVALID_FLAGS,
                                               "children_use_socket requires async_accept",
                                               &*socketId));
        }

        if (request.has_children_async_data() && request.children_async_data()) {
            return send(makeSocketReplyMessage(message,
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
            return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_IN_USE, "socket id already exists", &*socketId));
        }
    }

    if (!tcpTransport->listen(request.host_port())) {
        tcpTransport->stop();
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "tcp listen failed", &*socketId));
    }

    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedListeningSockets_.emplace(*socketId, listenerState);
    }

    const bool sent = send(makeSocketReplyMessage(message, rsp::proto::SUCCESS, std::string(), &*socketId));
    if (!sent) {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedListeningSockets_.erase(*socketId);
        stopManagedListener(listenerState);
        return false;
    }

    return true;
}

bool BsdSocketsResourceService::handleAcceptTCP(const rsp::proto::RSPMessage& message) {
    const auto& request = message.accept_tcp();
    const auto listenSocketId = fromProtoSocketId(request.listen_socket_number());
    if (!listenSocketId.has_value()) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "invalid listen socket id"));
    }

    std::shared_ptr<ManagedListenerState> listenerState;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        const auto iterator = managedListeningSockets_.find(*listenSocketId);
        if (iterator == managedListeningSockets_.end()) {
            return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_CLOSED, "listen socket not found", &*listenSocketId));
        }

        listenerState = iterator->second;
    }

    if (!validateListeningSocketAccess(message, listenerState)) {
        return send(makeSocketReplyMessage(message,
                                           rsp::proto::NODEID_MISMATCH,
                                           "listen socket is owned by a different node id",
                                           &*listenSocketId));
    }

    if (listenerState->asyncAccept) {
        return send(makeSocketReplyMessage(message,
                                           rsp::proto::ASYNC_SOCKET,
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
                return send(makeSocketReplyMessage(message, rsp::proto::TIMED_OUT, "accept timed out", &*listenSocketId));
            }
        }

        if (listenerState->stopping.load() || listenerState->acceptedConnections.empty()) {
            return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_CLOSED, "listen socket closed", &*listenSocketId));
        }

        acceptedConnection = listenerState->acceptedConnections.front();
        listenerState->acceptedConnections.pop_front();
    }

    const auto childSocketId = request.has_new_socket_number()
                                   ? fromProtoSocketId(request.new_socket_number())
                                   : std::optional<rsp::GUID>(rsp::GUID());
    if (!childSocketId.has_value()) {
        if (acceptedConnection != nullptr) {
            acceptedConnection->close();
        }
        return send(makeSocketReplyMessage(message, rsp::proto::INVALID_FLAGS, "invalid new_socket_number", &*listenSocketId));
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
            return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_IN_USE, "socket id already exists", &*listenSocketId));
        }

        managedSockets_.emplace(*childSocketId, socketState);
    }

    auto reply = makeSocketReplyMessage(message, rsp::proto::SUCCESS, std::string(), &*listenSocketId);
    *reply.mutable_socket_reply()->mutable_new_socket_id() = toProtoSocketId(*childSocketId);
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

bool BsdSocketsResourceService::handleSocketSend(const rsp::proto::RSPMessage& message) {
    const bool trace = rsp::messageTraceEnabled(message);
    const auto socketId = fromProtoSocketId(message.socket_send().socket_number());
    if (!socketId.has_value()) {
        if (trace) {
            std::cerr << "[resource_service] socket_send invalid socket id" << std::endl;
        }
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "invalid socket id"));
    }

    std::shared_ptr<ManagedSocketState> socketState;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        const auto iterator = managedSockets_.find(*socketId);
        if (iterator == managedSockets_.end()) {
            if (trace) {
                std::cerr << "[resource_service] socket_send socket not found id=" << socketId->toString() << std::endl;
            }
            return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_CLOSED, "socket not found"));
        }

        socketState = iterator->second;
    }

    if (!validateSocketAccess(message, socketState)) {
        if (trace || (socketState != nullptr && socketState->traceEnabled)) {
            std::cerr << "[resource_service] socket_send node mismatch id=" << socketId->toString() << std::endl;
        }
        return send(makeSocketReplyMessage(message, rsp::proto::NODEID_MISMATCH,
                                           "socket is owned by a different node id"));
    }

    const bool effectiveTrace = trace || (socketState != nullptr && socketState->traceEnabled);

    const std::string& data = message.socket_send().data();
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
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "socket send failed"));
    }

    if (effectiveTrace) {
        std::cerr << "[resource_service] socket_send success id=" << socketId->toString() << std::endl;
    }
    return send(makeSocketReplyMessage(message, rsp::proto::SUCCESS));
}

bool BsdSocketsResourceService::handleSocketRecv(const rsp::proto::RSPMessage& message) {
    const auto socketId = fromProtoSocketId(message.socket_recv().socket_number());
    if (!socketId.has_value()) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "invalid socket id"));
    }

    std::shared_ptr<ManagedSocketState> socketState;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        const auto iterator = managedSockets_.find(*socketId);
        if (iterator == managedSockets_.end()) {
            return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_CLOSED, "socket not found"));
        }

        socketState = iterator->second;
    }

    if (!validateSocketAccess(message, socketState)) {
        return send(makeSocketReplyMessage(message, rsp::proto::NODEID_MISMATCH,
                                           "socket is owned by a different node id"));
    }

    if (socketState != nullptr && socketState->asyncData) {
        return send(makeSocketReplyMessage(message, rsp::proto::ASYNC_SOCKET,
                                           "socket_recv is ignored when async_data is enabled"));
    }

    const uint32_t maxBytes = message.socket_recv().has_max_bytes() && message.socket_recv().max_bytes() > 0
                                  ? message.socket_recv().max_bytes()
                                  : 4096u;
    rsp::Buffer buffer(maxBytes);
    const int bytesRead = socketState == nullptr || socketState->connection == nullptr ? -1 : socketState->connection->recv(buffer);
    if (bytesRead < 0) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "socket recv failed"));
    }

    if (bytesRead == 0) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_CLOSED, "socket closed"));
    }

    auto reply = makeSocketReplyMessage(message, rsp::proto::SOCKET_DATA);
    reply.mutable_socket_reply()->set_data(readBufferToString(buffer, static_cast<uint32_t>(bytesRead)));
    return send(reply);
}

bool BsdSocketsResourceService::handleSocketClose(const rsp::proto::RSPMessage& message) {
    const auto socketId = fromProtoSocketId(message.socket_close().socket_number());
    if (!socketId.has_value()) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "invalid socket id"));
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
            return send(makeSocketReplyMessage(message,
                                               rsp::proto::NODEID_MISMATCH,
                                               "listen socket is owned by a different node id",
                                               &*socketId));
        }

        stopManagedListener(removedListener);
        return send(makeSocketReplyMessage(message, rsp::proto::SUCCESS, std::string(), &*socketId));
    }

    std::shared_ptr<ManagedSocketState> removedSocket;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        const auto iterator = managedSockets_.find(*socketId);
        if (iterator == managedSockets_.end()) {
            return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_CLOSED, "socket not found"));
        }

        removedSocket = iterator->second;
        managedSockets_.erase(iterator);
    }

    if (!validateSocketAccess(message, removedSocket)) {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedSockets_.emplace(*socketId, removedSocket);
        return send(makeSocketReplyMessage(message, rsp::proto::NODEID_MISMATCH,
                                           "socket is owned by a different node id"));
    }

    stopManagedSocket(removedSocket);

    return send(makeSocketReplyMessage(message, rsp::proto::SUCCESS));
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

    auto reply = makeSocketReplyMessage(listenerState->requesterNodeId,
                                        rsp::proto::NEW_CONNECTION,
                                        std::string(),
                                        &listenerState->socketId,
                                        listenerState->traceEnabled);
    *reply.mutable_socket_reply()->mutable_new_socket_id() = toProtoSocketId(socketState->socketId);
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
                const auto reply = makeSocketReplyMessage(socketState->requesterNodeId,
                                                          rsp::proto::SOCKET_ERROR,
                                                          "socket recv failed",
                                                          &socketState->socketId,
                                                          socketState->traceEnabled);
                if (socketState->traceEnabled) {
                    std::cerr << "[resource_service] async_read_loop send SOCKET_ERROR id="
                              << socketState->socketId.toString() << std::endl;
                }
                send(reply);
            }
            break;
        }

        if (bytesRead == 0) {
            if (!socketState->stopping.load()) {
                const auto reply = makeSocketReplyMessage(socketState->requesterNodeId,
                                                          rsp::proto::SOCKET_CLOSED,
                                                          "socket closed",
                                                          &socketState->socketId,
                                                          socketState->traceEnabled);
                if (socketState->traceEnabled) {
                    std::cerr << "[resource_service] async_read_loop send SOCKET_CLOSED id="
                              << socketState->socketId.toString() << std::endl;
                }
                send(reply);
            }
            break;
        }

        auto reply = makeSocketReplyMessage(socketState->requesterNodeId,
                                            rsp::proto::SOCKET_DATA,
                                            std::string(),
                                            &socketState->socketId,
                                            socketState->traceEnabled);
        reply.mutable_socket_reply()->set_data(readBufferToString(buffer, static_cast<uint32_t>(bytesRead)));
        if (socketState->traceEnabled) {
            std::cerr << "[resource_service] async_read_loop send SOCKET_DATA id="
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

rsp::proto::RSPMessage BsdSocketsResourceService::makeSocketReplyMessage(const rsp::proto::RSPMessage& request,
                                                                         rsp::proto::SOCKET_STATUS status,
                                                                         const std::string& errorMessage,
                                                                         const rsp::GUID* socketId) const {
    const rsp::GUID* effectiveSocketId = socketId;
    std::optional<rsp::GUID> derivedSocketId;
    if (effectiveSocketId == nullptr) {
        if (request.has_connect_tcp_request() && request.connect_tcp_request().has_socket_number()) {
            derivedSocketId = fromProtoSocketId(request.connect_tcp_request().socket_number());
        } else if (request.has_listen_tcp_request() && request.listen_tcp_request().has_socket_number()) {
            derivedSocketId = fromProtoSocketId(request.listen_tcp_request().socket_number());
        } else if (request.has_accept_tcp()) {
            derivedSocketId = fromProtoSocketId(request.accept_tcp().listen_socket_number());
        } else if (request.has_socket_send()) {
            derivedSocketId = fromProtoSocketId(request.socket_send().socket_number());
        } else if (request.has_socket_recv()) {
            derivedSocketId = fromProtoSocketId(request.socket_recv().socket_number());
        } else if (request.has_socket_close()) {
            derivedSocketId = fromProtoSocketId(request.socket_close().socket_number());
        }

        if (derivedSocketId.has_value()) {
            effectiveSocketId = &*derivedSocketId;
        }
    }

    const auto senderNodeId = rsp::senderNodeIdFromMessage(request);
    if (!senderNodeId.has_value()) {
        return rsp::proto::RSPMessage();
    }

    auto reply = makeSocketReplyMessage(toProtoNodeId(*senderNodeId), status, errorMessage, effectiveSocketId);
    rsp::copyMessageTrace(request, reply);
    return reply;
}

rsp::proto::RSPMessage BsdSocketsResourceService::makeSocketReplyMessage(const rsp::proto::NodeId& destinationNodeId,
                                                                         rsp::proto::SOCKET_STATUS status,
                                                                         const std::string& errorMessage,
                                                                         const rsp::GUID* socketId,
                                                                         bool traceEnabled) const {
    rsp::proto::RSPMessage reply;
    *reply.mutable_destination() = destinationNodeId;
    if (traceEnabled) {
        reply.mutable_trace()->set_value(true);
    }
    if (socketId != nullptr) {
        *reply.mutable_socket_reply()->mutable_socket_id() = toProtoSocketId(*socketId);
    }
    reply.mutable_socket_reply()->set_error(status);
    if (!errorMessage.empty()) {
        reply.mutable_socket_reply()->set_message(errorMessage);
    }
    if (socketId != nullptr) {
        *reply.mutable_socket_reply()->mutable_new_socket_id() = toProtoSocketId(*socketId);
    }

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

rsp::proto::SocketID BsdSocketsResourceService::toProtoSocketId(const rsp::GUID& socketId) {
    rsp::proto::SocketID protoSocketId;
    std::string value(16, '\0');
    const uint64_t high = socketId.high();
    const uint64_t low = socketId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoSocketId.set_value(value);
    return protoSocketId;
}

std::optional<rsp::GUID> BsdSocketsResourceService::fromProtoSocketId(const rsp::proto::SocketID& socketId) {
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
