#include "os/os_socket.hpp"

#include <atomic>
#include <cstdint>
#include <set>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <iphlpapi.h>
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

void insertAddress(const sockaddr* address, std::set<IPAddress>& addresses) {
    if (address == nullptr) {
        return;
    }

    if (address->sa_family == AF_INET) {
        const auto* ipv4Address = reinterpret_cast<const sockaddr_in*>(address);
        const uint32_t hostAddress = ntohl(ipv4Address->sin_addr.s_addr);
        if ((hostAddress >> 24) == 127 || hostAddress == 0) {
            return;
        }

        IPAddress entry;
        entry.family = IPAddressFamily::IPv4;
        entry.ipv4 = hostAddress;
        addresses.insert(entry);
        return;
    }

    if (address->sa_family == AF_INET6) {
        const auto* ipv6Address = reinterpret_cast<const sockaddr_in6*>(address);
        if (IN6_IS_ADDR_LOOPBACK(&ipv6Address->sin6_addr) || IN6_IS_ADDR_UNSPECIFIED(&ipv6Address->sin6_addr)) {
            return;
        }

        IPAddress entry;
        entry.family = IPAddressFamily::IPv6;
        std::memcpy(entry.ipv6.data(), &ipv6Address->sin6_addr, entry.ipv6.size());
        addresses.insert(entry);
    }
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

bool createSocketPair(SocketHandle& firstSocket, SocketHandle& secondSocket) {
    firstSocket = invalidSocket();
    secondSocket = invalidSocket();

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        closesocket(listener);
        return false;
    }

    int addressLength = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
        closesocket(listener);
        return false;
    }

    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET) {
        closesocket(listener);
        return false;
    }

    if (connect(clientSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(clientSocket);
        closesocket(listener);
        return false;
    }

    SOCKET serverSocket = accept(listener, nullptr, nullptr);
    closesocket(listener);
    if (serverSocket == INVALID_SOCKET) {
        closesocket(clientSocket);
        return false;
    }

    firstSocket = fromNative(clientSocket);
    secondSocket = fromNative(serverSocket);
    return true;
}

bool createLocalListenerSocket(SocketHandle& listenerSocket, std::string& endpoint, int backlog) {
    listenerSocket = invalidSocket();

    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(socketHandle, backlog) != 0) {
        closesocket(socketHandle);
        return false;
    }

    int addressLength = sizeof(address);
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
        closesocket(socketHandle);
        return false;
    }

    listenerSocket = fromNative(socketHandle);
    endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    return true;
}

SocketHandle connectLocalListenerSocket(const std::string& endpoint) {
    const auto separator = endpoint.rfind(':');
    if (separator == std::string::npos) {
        return invalidSocket();
    }

    const std::string address = endpoint.substr(0, separator);
    const auto portValue = std::stoul(endpoint.substr(separator + 1));
    if (portValue > UINT16_MAX) {
        return invalidSocket();
    }

    return connectTcp(address, static_cast<uint16_t>(portValue));
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

uint16_t getSocketPort(SocketHandle socketHandle) {
    sockaddr_storage address = {};
    int addressLength = sizeof(address);
    if (getsockname(toNative(socketHandle), reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
        return 0;
    }

    if (address.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
    }

    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    }

    return 0;
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

std::vector<IPAddress> listNonLocalAddresses() {
    ULONG bufferSize = 16 * 1024;
    std::vector<uint8_t> buffer(bufferSize);
    IP_ADAPTER_ADDRESSES* adapterAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG result = GetAdaptersAddresses(AF_UNSPEC,
                                        GAA_FLAG_SKIP_ANYCAST |
                                            GAA_FLAG_SKIP_MULTICAST |
                                            GAA_FLAG_SKIP_DNS_SERVER,
                                        nullptr,
                                        adapterAddresses,
                                        &bufferSize);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferSize);
        adapterAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_UNSPEC,
                                      GAA_FLAG_SKIP_ANYCAST |
                                          GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr,
                                      adapterAddresses,
                                      &bufferSize);
    }

    if (result != NO_ERROR) {
        return {};
    }

    std::set<IPAddress> addresses;
    for (IP_ADAPTER_ADDRESSES* adapter = adapterAddresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr;
             unicast = unicast->Next) {
            insertAddress(unicast->Address.lpSockaddr, addresses);
        }
    }

    return {addresses.begin(), addresses.end()};
}

void closeSocket(SocketHandle socketHandle) {
    if (isValidSocket(socketHandle)) {
        shutdown(toNative(socketHandle), SD_BOTH);
        closesocket(toNative(socketHandle));
    }
}

}  // namespace rsp::os