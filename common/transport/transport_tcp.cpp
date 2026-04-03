#include "common/transport/transport_tcp.hpp"

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

TcpConnection::TcpConnection(rsp::os::SocketHandle socketHandle) : socketHandle_(socketHandle) {
}

TcpConnection::~TcpConnection() {
    close();
}

int TcpConnection::send(const rsp::Buffer& data) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    rsp::os::SocketHandle socketHandle = rsp::os::invalidSocket();
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        socketHandle = socketHandle_;
    }

    if (!rsp::os::isValidSocket(socketHandle)) {
        return -1;
    }

    return rsp::os::sendSocket(socketHandle, data.data(), data.size());
}

int TcpConnection::recv(rsp::Buffer& buffer) {
    std::lock_guard<std::mutex> recvLock(recvMutex_);
    rsp::os::SocketHandle socketHandle = rsp::os::invalidSocket();
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        socketHandle = socketHandle_;
    }

    if (!rsp::os::isValidSocket(socketHandle)) {
        return -1;
    }

    return rsp::os::recvSocket(socketHandle, buffer.data(), buffer.size());
}

void TcpConnection::close() {
    rsp::os::SocketHandle socketHandle = rsp::os::invalidSocket();
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        socketHandle = socketHandle_;
        socketHandle_ = rsp::os::invalidSocket();
    }

    if (rsp::os::isValidSocket(socketHandle)) {
        rsp::os::closeSocket(socketHandle);
    }
}

TcpTransport::TcpTransport() : listener_(rsp::os::invalidSocket()), listening_(false) {
    if (!rsp::os::initializeSockets()) {
        throw std::runtime_error("failed to initialize socket subsystem");
    }
}

TcpTransport::~TcpTransport() {
    stop();
    rsp::os::shutdownSockets();
}

bool TcpTransport::listen(const std::string& parameters) {
    std::string bindAddress;
    uint16_t port = 0;
    if (!parseEndpoint(parameters, bindAddress, port)) {
        return false;
    }

    const rsp::os::SocketHandle listener = rsp::os::createTcpListener(bindAddress, port, 16);
    if (!rsp::os::isValidSocket(listener)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (listening_) {
            rsp::os::closeSocket(listener);
            return false;
        }

        listener_ = listener;
        listening_ = true;
    }

    acceptThread_ = std::thread(&TcpTransport::acceptLoop, this);
    return true;
}

ConnectionHandle TcpTransport::connect(const std::string& parameters) {
    std::string address;
    uint16_t port = 0;
    if (!parseEndpoint(parameters, address, port)) {
        return nullptr;
    }

    ConnectionHandle newConnection = connectParsed(address, port);
    if (newConnection == nullptr) {
        return nullptr;
    }

    ConnectionHandle previousConnection;
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastParameters_ = parameters;
    previousConnection = activeConnection_;
    activeConnection_ = newConnection;

    if (previousConnection != nullptr && previousConnection != newConnection) {
        previousConnection->close();
    }

    return newConnection;
}

ConnectionHandle TcpTransport::reconnect() {
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

void TcpTransport::stop() {
    stopListening();
    stopConnection();
}

ConnectionHandle TcpTransport::connection() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return activeConnection_;
}

bool TcpTransport::parseEndpoint(const std::string& parameters, std::string& address, uint16_t& port) {
    const size_t separator = parameters.rfind(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= parameters.size()) {
        return false;
    }

    address = parameters.substr(0, separator);
    return parsePort(parameters.substr(separator + 1), port);
}

void TcpTransport::stopListening() {
    rsp::os::SocketHandle listener = rsp::os::invalidSocket();

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        listening_ = false;
        listener = listener_;
        listener_ = rsp::os::invalidSocket();
    }

    if (rsp::os::isValidSocket(listener)) {
        rsp::os::closeSocket(listener);
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
}

void TcpTransport::stopConnection() {
    ConnectionHandle activeConnection;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        activeConnection = std::move(activeConnection_);
    }

    if (activeConnection != nullptr) {
        activeConnection->close();
    }
}

void TcpTransport::acceptLoop() {
    while (true) {
        rsp::os::SocketHandle listener = rsp::os::invalidSocket();

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!listening_) {
                break;
            }

            listener = listener_;
        }

        const rsp::os::SocketHandle acceptedSocket = rsp::os::acceptSocket(listener);
        if (!rsp::os::isValidSocket(acceptedSocket)) {
            continue;
        }

        notifyNewConnection(std::make_shared<TcpConnection>(acceptedSocket));
    }
}

ConnectionHandle TcpTransport::connectParsed(const std::string& address, uint16_t port) {
    const rsp::os::SocketHandle socketHandle = rsp::os::connectTcp(address, port);
    if (!rsp::os::isValidSocket(socketHandle)) {
        return nullptr;
    }

    return std::make_shared<TcpConnection>(socketHandle);
}

}  // namespace rsp::transport