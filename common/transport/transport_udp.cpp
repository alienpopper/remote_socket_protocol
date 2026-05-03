#include "common/transport/transport_udp.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace rsp::transport {

namespace {

bool parsePort(const std::string& text, uint16_t& port) {
    if (text.empty()) {
        return false;
    }

    unsigned long value = 0;
    for (const char current : text) {
        if (current < '0' || current > '9') {
            return false;
        }

        value = (value * 10UL) + static_cast<unsigned long>(current - '0');
        if (value > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
    }

    port = static_cast<uint16_t>(value);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// UdpServerConnection
// ---------------------------------------------------------------------------

UdpServerConnection::UdpServerConnection(rsp::os::SocketHandle sharedSocket,
                                         rsp::os::PeerAddress peerAddress)
    : sharedSocket_(sharedSocket), peerAddress_(std::move(peerAddress)) {
}

UdpServerConnection::~UdpServerConnection() {
    close();
}

int UdpServerConnection::send(const rsp::Buffer& data) {
    return rsp::os::sendSocketTo(sharedSocket_, data.data(), data.size(), peerAddress_);
}

int UdpServerConnection::recv(rsp::Buffer& buffer) {
    std::unique_lock<std::mutex> lock(queueMutex_);
    queueCondition_.wait(lock, [this] { return closed_ || !queue_.empty(); });

    if (closed_ && queue_.empty()) {
        return -1;
    }

    std::vector<uint8_t> datagram = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();

    buffer.resize(static_cast<uint32_t>(datagram.size()));
    if (!datagram.empty()) {
        std::memcpy(buffer.data(), datagram.data(), datagram.size());
    }
    return static_cast<int>(datagram.size());
}

void UdpServerConnection::close() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        closed_ = true;
    }
    queueCondition_.notify_all();
}

void UdpServerConnection::enqueue(std::vector<uint8_t> datagram) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (closed_) {
            return;
        }
        queue_.push_back(std::move(datagram));
    }
    queueCondition_.notify_one();
}

// ---------------------------------------------------------------------------
// UdpClientConnection
// ---------------------------------------------------------------------------

UdpClientConnection::UdpClientConnection(rsp::os::SocketHandle socketHandle)
    : socketHandle_(socketHandle) {
}

UdpClientConnection::~UdpClientConnection() {
    close();
}

int UdpClientConnection::send(const rsp::Buffer& data) {
    rsp::os::SocketHandle socketHandle = rsp::os::invalidSocket();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        socketHandle = socketHandle_;
    }

    if (!rsp::os::isValidSocket(socketHandle)) {
        return -1;
    }

    return rsp::os::sendSocket(socketHandle, data.data(), data.size());
}

int UdpClientConnection::recv(rsp::Buffer& buffer) {
    rsp::os::SocketHandle socketHandle = rsp::os::invalidSocket();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        socketHandle = socketHandle_;
    }

    if (!rsp::os::isValidSocket(socketHandle)) {
        return -1;
    }

    // Use a staging buffer to capture the full datagram regardless of its size,
    // then copy into (and resize) the caller's buffer.
    std::vector<uint8_t> staging(kUdpMaxDatagramSize);
    const int bytesRead = rsp::os::recvSocket(socketHandle, staging.data(),
                                               static_cast<uint32_t>(staging.size()));
    if (bytesRead <= 0) {
        return bytesRead;
    }

    buffer.resize(static_cast<uint32_t>(bytesRead));
    std::memcpy(buffer.data(), staging.data(), static_cast<size_t>(bytesRead));
    return bytesRead;
}

void UdpClientConnection::close() {
    rsp::os::SocketHandle socketHandle = rsp::os::invalidSocket();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        socketHandle = socketHandle_;
        socketHandle_ = rsp::os::invalidSocket();
    }

    if (rsp::os::isValidSocket(socketHandle)) {
        rsp::os::closeSocket(socketHandle);
    }
}

// ---------------------------------------------------------------------------
// UdpTransport
// ---------------------------------------------------------------------------

UdpTransport::UdpTransport() : listenerSocket_(rsp::os::invalidSocket()) {
    if (!rsp::os::initializeSockets()) {
        throw std::runtime_error("failed to initialize socket subsystem");
    }
}

UdpTransport::~UdpTransport() {
    stop();
    rsp::os::shutdownSockets();
}

bool UdpTransport::listen(const std::string& parameters) {
    std::string bindAddress;
    uint16_t port = 0;
    if (!parseEndpoint(parameters, bindAddress, port)) {
        return false;
    }

    const rsp::os::SocketHandle listener = rsp::os::createUdpSocket(bindAddress, port);
    if (!rsp::os::isValidSocket(listener)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (listening_) {
            rsp::os::closeSocket(listener);
            return false;
        }

        listenerSocket_ = listener;
        listening_ = true;
    }

    demuxThread_ = std::thread(&UdpTransport::demuxLoop, this);
    return true;
}

uint16_t UdpTransport::listenedPort() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!rsp::os::isValidSocket(listenerSocket_)) {
        return 0;
    }
    return rsp::os::getSocketPort(listenerSocket_);
}

ConnectionHandle UdpTransport::connect(const std::string& parameters) {
    std::string address;
    uint16_t port = 0;
    if (!parseEndpoint(parameters, address, port)) {
        return nullptr;
    }

    const rsp::os::SocketHandle socketHandle = rsp::os::connectUdp(address, port);
    if (!rsp::os::isValidSocket(socketHandle)) {
        return nullptr;
    }

    auto newConnection = std::make_shared<UdpClientConnection>(socketHandle);

    ConnectionHandle previousConnection;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastParameters_ = parameters;
        previousConnection = activeConnection_;
        activeConnection_ = newConnection;
    }

    if (previousConnection != nullptr && previousConnection != newConnection) {
        previousConnection->close();
    }

    return newConnection;
}

ConnectionHandle UdpTransport::reconnect() {
    std::string parameters;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        parameters = lastParameters_;
    }

    if (parameters.empty()) {
        return nullptr;
    }

    return connect(parameters);
}

void UdpTransport::stop() {
    stopListening();
    stopConnection();
}

ConnectionHandle UdpTransport::connection() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return activeConnection_;
}

bool UdpTransport::parseEndpoint(const std::string& parameters,
                                  std::string& address, uint16_t& port) {
    const size_t separator = parameters.rfind(':');
    if (separator == std::string::npos || separator + 1 >= parameters.size()) {
        return false;
    }

    address = parameters.substr(0, separator);
    return parsePort(parameters.substr(separator + 1), port);
}

void UdpTransport::stopListening() {
    rsp::os::SocketHandle listener = rsp::os::invalidSocket();

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        listening_ = false;
        listener = listenerSocket_;
        listenerSocket_ = rsp::os::invalidSocket();
    }

    // Closing the socket unblocks any recvSocketFrom in the demux thread.
    if (rsp::os::isValidSocket(listener)) {
        rsp::os::closeSocket(listener);
    }

    if (demuxThread_.joinable()) {
        demuxThread_.join();
    }

    // Close all server-side connections so their recv() calls unblock.
    std::lock_guard<std::mutex> lock(stateMutex_);
    for (auto& [key, weakConn] : peers_) {
        if (auto conn = weakConn.lock()) {
            conn->close();
        }
    }
    peers_.clear();
}

void UdpTransport::stopConnection() {
    ConnectionHandle activeConnection;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        activeConnection = std::move(activeConnection_);
    }

    if (activeConnection != nullptr) {
        activeConnection->close();
    }
}

void UdpTransport::demuxLoop() {
    std::vector<uint8_t> staging(kUdpMaxDatagramSize);

    while (true) {
        rsp::os::SocketHandle listener = rsp::os::invalidSocket();
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!listening_) {
                break;
            }
            listener = listenerSocket_;
        }

        rsp::os::PeerAddress source;
        const int bytesRead = rsp::os::recvSocketFrom(
            listener, staging.data(), static_cast<uint32_t>(staging.size()), source);

        if (bytesRead <= 0) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!listening_) {
                break;
            }
            continue;
        }

        const std::string key = rsp::os::peerAddressKey(source);
        std::vector<uint8_t> datagram(staging.begin(), staging.begin() + bytesRead);

        std::shared_ptr<UdpServerConnection> conn;
        bool isNew = false;

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            auto it = peers_.find(key);
            if (it != peers_.end()) {
                conn = it->second.lock();
            }

            if (conn == nullptr) {
                conn = std::make_shared<UdpServerConnection>(listenerSocket_, source);
                peers_[key] = conn;
                isNew = true;
            }
        }

        conn->enqueue(std::move(datagram));

        // Notify outside the lock — the callback may acquire its own locks.
        if (isNew) {
            notifyNewConnection(conn);
        }
    }
}

}  // namespace rsp::transport
