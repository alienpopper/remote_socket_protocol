#include "os/os_socket.hpp"

#include <atomic>
#include <string>

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

namespace rsp::os {

namespace {

std::atomic<uint32_t> g_socketUsers = 0;

SOCKET toNative(SocketHandle socketHandle) {
    return static_cast<SOCKET>(socketHandle);
}

SocketHandle fromNative(SOCKET socketHandle) {
    return static_cast<SocketHandle>(socketHandle);
}

std::string portString(uint16_t port) {
    return std::to_string(port);
}

}  // namespace

bool initializeSockets() {
    if (g_socketUsers.fetch_add(1) == 0) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            g_socketUsers.fetch_sub(1);
            return false;
        }
    }

    return true;
}

void shutdownSockets() {
    const uint32_t previousUsers = g_socketUsers.fetch_sub(1);
    if (previousUsers == 1) {
        WSACleanup();
    }
}

SocketHandle invalidSocket() {
    return fromNative(INVALID_SOCKET);
}

bool isValidSocket(SocketHandle socketHandle) {
    return toNative(socketHandle) != INVALID_SOCKET;
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
        SOCKET socketHandle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socketHandle == INVALID_SOCKET) {
            continue;
        }

        if (bind(socketHandle, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0 &&
            listen(socketHandle, backlog) == 0) {
            listener = fromNative(socketHandle);
            break;
        }

        closesocket(socketHandle);
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
        SOCKET socketHandle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socketHandle == INVALID_SOCKET) {
            continue;
        }

        if (connect(socketHandle, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
            connectedSocket = fromNative(socketHandle);
            break;
        }

        closesocket(socketHandle);
    }

    freeaddrinfo(result);
    return connectedSocket;
}

int sendSocket(SocketHandle socketHandle, const uint8_t* data, uint32_t length) {
    return send(toNative(socketHandle), reinterpret_cast<const char*>(data), static_cast<int>(length), 0);
}

int recvSocket(SocketHandle socketHandle, uint8_t* buffer, uint32_t length) {
    return recv(toNative(socketHandle), reinterpret_cast<char*>(buffer), static_cast<int>(length), 0);
}

void closeSocket(SocketHandle socketHandle) {
    if (isValidSocket(socketHandle)) {
        shutdown(toNative(socketHandle), SD_BOTH);
        closesocket(toNative(socketHandle));
    }
}

}  // namespace rsp::os