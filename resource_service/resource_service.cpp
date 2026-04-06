#include "resource_service/resource_service.hpp"

#include "common/transport/transport_tcp.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

namespace rsp::resource_service {

namespace {

rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoNodeId.set_value(value);
    return protoNodeId;
}

std::string readBufferToString(const rsp::Buffer& buffer, uint32_t length) {
    return std::string(reinterpret_cast<const char*>(buffer.data()), length);
}

}  // namespace

ResourceService::Ptr ResourceService::create() {
    return Ptr(new ResourceService(KeyPair::generateP256()));
}

ResourceService::Ptr ResourceService::create(KeyPair keyPair) {
    return Ptr(new ResourceService(std::move(keyPair)));
}

ResourceService::ResourceService(KeyPair keyPair)
    : rsp::client::full::RSPClient(std::move(keyPair)) {
}

ResourceService::~ResourceService() {
    closeAllManagedSockets();
}

bool ResourceService::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_connect_tcp_request()) {
        return handleConnectTCPRequest(message);
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

bool ResourceService::handleConnectTCPRequest(const rsp::proto::RSPMessage& message) {
    if (!message.has_source()) {
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

    rsp::transport::TransportHandle transport;
    rsp::transport::ConnectionHandle connection;
    for (uint32_t attempt = 0; attempt < totalAttempts; ++attempt) {
        auto tcpTransport = std::make_shared<rsp::transport::TcpTransport>();
        connection = tcpTransport->connect(request.host_port());
        if (connection != nullptr) {
            transport = std::move(tcpTransport);
            break;
        }

        tcpTransport->stop();
        if (attempt + 1u < totalAttempts && retryDelayMilliseconds > 0u) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMilliseconds));
        }
    }

    if (connection == nullptr || transport == nullptr) {
        return send(makeSocketReplyMessage(message, rsp::proto::CONNECT_REFUSED, "tcp connect failed", &*socketId));
    }

    auto socketState = std::make_shared<ManagedSocketState>();
    socketState->transport = transport;
    socketState->connection = connection;
    socketState->requesterNodeId = message.source();
    socketState->socketId = *socketId;
    socketState->asyncData = request.has_async_data() && request.async_data();
    socketState->shareSocket = request.has_share_socket() && request.share_socket();
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedSockets_.emplace(*socketId, socketState);
    }

    const bool sent = send(makeSocketReplyMessage(message, rsp::proto::SUCCESS, std::string(), &*socketId));
    if (!sent) {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        managedSockets_.erase(*socketId);
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

bool ResourceService::handleSocketSend(const rsp::proto::RSPMessage& message) {
    const auto socketId = fromProtoSocketId(message.socket_send().socket_number());
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

    const std::string& data = message.socket_send().data();
    const bool sent = socketState != nullptr && socketState->connection != nullptr &&
                      socketState->connection->sendAll(reinterpret_cast<const uint8_t*>(data.data()),
                                                      static_cast<uint32_t>(data.size()));
    if (!sent) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "socket send failed"));
    }

    return send(makeSocketReplyMessage(message, rsp::proto::SUCCESS));
}

bool ResourceService::handleSocketRecv(const rsp::proto::RSPMessage& message) {
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

bool ResourceService::handleSocketClose(const rsp::proto::RSPMessage& message) {
    const auto socketId = fromProtoSocketId(message.socket_close().socket_number());
    if (!socketId.has_value()) {
        return send(makeSocketReplyMessage(message, rsp::proto::SOCKET_ERROR, "invalid socket id"));
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

void ResourceService::runAsyncReadLoop(const std::shared_ptr<ManagedSocketState>& socketState) {
    if (socketState == nullptr || socketState->connection == nullptr) {
        return;
    }

    while (!socketState->stopping.load()) {
        rsp::Buffer buffer(4096);
        const int bytesRead = socketState->connection->recv(buffer);
        if (bytesRead < 0) {
            if (!socketState->stopping.load()) {
                const auto reply = makeSocketReplyMessage(socketState->requesterNodeId,
                                                          rsp::proto::SOCKET_ERROR,
                                                          "socket recv failed",
                                                          &socketState->socketId);
                send(reply);
            }
            break;
        }

        if (bytesRead == 0) {
            if (!socketState->stopping.load()) {
                const auto reply = makeSocketReplyMessage(socketState->requesterNodeId,
                                                          rsp::proto::SOCKET_CLOSED,
                                                          "socket closed",
                                                          &socketState->socketId);
                send(reply);
            }
            break;
        }

        auto reply = makeSocketReplyMessage(socketState->requesterNodeId,
                                            rsp::proto::SOCKET_DATA,
                                            std::string(),
                                            &socketState->socketId);
        reply.mutable_socket_reply()->set_data(readBufferToString(buffer, static_cast<uint32_t>(bytesRead)));
        if (!send(reply)) {
            break;
        }
    }
}

bool ResourceService::validateSocketAccess(const rsp::proto::RSPMessage& message,
                                           const std::shared_ptr<ManagedSocketState>& socketState) const {
    if (socketState == nullptr || socketState->shareSocket) {
        return true;
    }

    if (!message.has_source()) {
        return false;
    }

    return message.source().value() == socketState->requesterNodeId.value();
}

rsp::proto::RSPMessage ResourceService::makeSocketReplyMessage(const rsp::proto::RSPMessage& request,
                                                               rsp::proto::SOCKET_STATUS status,
                                                               const std::string& errorMessage,
                                                               const rsp::GUID* socketId) const {
    const rsp::GUID* effectiveSocketId = socketId;
    std::optional<rsp::GUID> derivedSocketId;
    if (effectiveSocketId == nullptr) {
        if (request.has_connect_tcp_request() && request.connect_tcp_request().has_socket_number()) {
            derivedSocketId = fromProtoSocketId(request.connect_tcp_request().socket_number());
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

    return makeSocketReplyMessage(request.source(), status, errorMessage, effectiveSocketId);
}

rsp::proto::RSPMessage ResourceService::makeSocketReplyMessage(const rsp::proto::NodeId& destinationNodeId,
                                                               rsp::proto::SOCKET_STATUS status,
                                                               const std::string& errorMessage,
                                                               const rsp::GUID* socketId) const {
    rsp::proto::RSPMessage reply;
    *reply.mutable_source() = toProtoNodeId(nodeId());
    *reply.mutable_destination() = destinationNodeId;
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

void ResourceService::stopManagedSocket(const std::shared_ptr<ManagedSocketState>& socketState) {
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

void ResourceService::closeAllManagedSockets() {
    std::map<rsp::GUID, std::shared_ptr<ManagedSocketState>> removedSockets;
    {
        std::lock_guard<std::mutex> lock(socketsMutex_);
        removedSockets.swap(managedSockets_);
    }

    for (auto& [_, socketState] : removedSockets) {
        stopManagedSocket(socketState);
    }
}

rsp::proto::SocketID ResourceService::toProtoSocketId(const rsp::GUID& socketId) {
    rsp::proto::SocketID protoSocketId;
    std::string value(16, '\0');
    const uint64_t high = socketId.high();
    const uint64_t low = socketId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoSocketId.set_value(value);
    return protoSocketId;
}

std::optional<rsp::GUID> ResourceService::fromProtoSocketId(const rsp::proto::SocketID& socketId) {
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