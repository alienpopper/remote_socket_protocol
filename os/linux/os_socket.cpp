#include "os/os_socket.hpp"

#include <cstring>
#include <set>
#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
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
        std::memcpy(entry.ipv6.data(), ipv6Address->sin6_addr.s6_addr, entry.ipv6.size());
        addresses.insert(entry);
    }
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

bool createSocketPair(SocketHandle& firstSocket, SocketHandle& secondSocket) {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        firstSocket = invalidSocket();
        secondSocket = invalidSocket();
        return false;
    }

    firstSocket = fromNative(sockets[0]);
    secondSocket = fromNative(sockets[1]);
    return true;
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

std::vector<IPAddress> listNonLocalAddresses() {
    std::set<IPAddress> addresses;
    ifaddrs* interfaceAddresses = nullptr;
    if (getifaddrs(&interfaceAddresses) != 0) {
        return {};
    }

    for (ifaddrs* current = interfaceAddresses; current != nullptr; current = current->ifa_next) {
        if (current->ifa_addr == nullptr || current->ifa_flags == 0) {
            continue;
        }

        if ((current->ifa_flags & IFF_UP) == 0 || (current->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        insertAddress(current->ifa_addr, addresses);
    }

    freeifaddrs(interfaceAddresses);
    return {addresses.begin(), addresses.end()};
}

void closeSocket(SocketHandle socketHandle) {
    if (isValidSocket(socketHandle)) {
        shutdown(toNative(socketHandle), SHUT_RDWR);
        close(toNative(socketHandle));
    }
}

}  // namespace rsp::os