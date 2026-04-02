#include "os/os_socket.hpp"

#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rsp::os {

namespace {

int toNative(SocketHandle socketHandle) {
    return static_cast<int>(socketHandle);
}

SocketHandle fromNative(int socketHandle) {
    return static_cast<SocketHandle>(socketHandle);
}

std::string portString(uint16_t port) {
    return std::to_string(port);
}

}  // namespace

bool initializeSockets() {
    return true;
}

void shutdownSockets() {
}

SocketHandle invalidSocket() {
    return static_cast<SocketHandle>(-1);
}

bool isValidSocket(SocketHandle socketHandle) {
    return toNative(socketHandle) >= 0;
}

SocketHandle createTcpListener(const std::string& bindAddress, uint16_t port, int backlog) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const char* host = bindAddress.empty() ? nullptr : bindAddress.c_str();
    if (getaddrinfo(host, portString(port).c_str(), &hints, &result) != 0) {
        return invalidSocket();
    }

    SocketHandle listener = invalidSocket();
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
        const int socketHandle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socketHandle < 0) {
            continue;
        }

        const int reuseAddress = 1;
        setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

        if (bind(socketHandle, current->ai_addr, current->ai_addrlen) == 0 &&
            listen(socketHandle, backlog) == 0) {
            listener = fromNative(socketHandle);
            break;
        }

        close(socketHandle);
    }

    freeaddrinfo(result);
    return listener;
}

SocketHandle acceptSocket(SocketHandle listener) {
    return fromNative(accept(toNative(listener), nullptr, nullptr));
}

SocketHandle connectTcp(const std::string& address, uint16_t port) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    if (getaddrinfo(address.c_str(), portString(port).c_str(), &hints, &result) != 0) {
        return invalidSocket();
    }

    SocketHandle connectedSocket = invalidSocket();
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
        const int socketHandle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socketHandle < 0) {
            continue;
        }

        if (connect(socketHandle, current->ai_addr, current->ai_addrlen) == 0) {
            connectedSocket = fromNative(socketHandle);
            break;
        }

        close(socketHandle);
    }

    freeaddrinfo(result);
    return connectedSocket;
}

int sendSocket(SocketHandle socketHandle, const uint8_t* data, uint32_t length) {
    return static_cast<int>(send(toNative(socketHandle), data, length, 0));
}

int recvSocket(SocketHandle socketHandle, uint8_t* buffer, uint32_t length) {
    return static_cast<int>(recv(toNative(socketHandle), buffer, length, 0));
}

void closeSocket(SocketHandle socketHandle) {
    if (isValidSocket(socketHandle)) {
        close(toNative(socketHandle));
    }
}

}  // namespace rsp::os