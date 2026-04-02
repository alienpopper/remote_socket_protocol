#include "common/transport/transport_tcp.hpp"

#include <limits>
#include <string>
#include <stdexcept>
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
    if (rsp::os::isValidSocket(socketHandle_)) {
        rsp::os::closeSocket(socketHandle_);
    }
}

TcpConnection::TcpConnection(TcpConnection&& other) noexcept : socketHandle_(other.socketHandle_) {
    other.socketHandle_ = rsp::os::invalidSocket();
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
    if (this != &other) {
        if (rsp::os::isValidSocket(socketHandle_)) {
            rsp::os::closeSocket(socketHandle_);
        }

        socketHandle_ = other.socketHandle_;
        other.socketHandle_ = rsp::os::invalidSocket();
    }

    return *this;
}

int TcpConnection::send(const rsp::Buffer& data) {
    return rsp::os::sendSocket(socketHandle_, data.data(), data.size());
}

int TcpConnection::recv(rsp::Buffer& buffer) {
    return rsp::os::recvSocket(socketHandle_, buffer.data(), buffer.size());
}

TcpTransport::TcpTransport() : listener_(rsp::os::invalidSocket()), listening_(false) {
    if (!rsp::os::initializeSockets()) {
        throw std::runtime_error("failed to initialize socket subsystem");
    }
}

TcpTransport::~TcpTransport() {
    stopListening();
    rsp::os::shutdownSockets();
}

bool TcpTransport::listen(const std::string& parameters) {
    if (listening_) {
        return false;
    }

    std::string bindAddress;
    uint16_t port = 0;
    if (!parseEndpoint(parameters, bindAddress, port)) {
        return false;
    }

    listener_ = rsp::os::createTcpListener(bindAddress, port, 16);
    if (!rsp::os::isValidSocket(listener_)) {
        return false;
    }

    listening_ = true;
    acceptThread_ = std::thread(&TcpTransport::acceptLoop, this);
    return true;
}

std::shared_ptr<Connection> TcpTransport::connect(const std::string& parameters) {
    std::string address;
    uint16_t port = 0;
    if (!parseEndpoint(parameters, address, port)) {
        return nullptr;
    }

    const rsp::os::SocketHandle socketHandle = rsp::os::connectTcp(address, port);
    if (!rsp::os::isValidSocket(socketHandle)) {
        return nullptr;
    }

    return std::make_shared<TcpConnection>(socketHandle);
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
    listening_ = false;

    if (rsp::os::isValidSocket(listener_)) {
        rsp::os::closeSocket(listener_);
        listener_ = rsp::os::invalidSocket();
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
}

void TcpTransport::acceptLoop() {
    while (listening_) {
        const rsp::os::SocketHandle acceptedSocket = rsp::os::acceptSocket(listener_);
        if (!rsp::os::isValidSocket(acceptedSocket)) {
            continue;
        }

        notifyNewConnection(std::make_shared<TcpConnection>(acceptedSocket));
    }
}

}  // namespace rsp::transport