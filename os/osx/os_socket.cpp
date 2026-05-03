#include "os/os_socket.hpp"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace rsp::os {

namespace {

int toNative(SocketHandle socketHandle) {
    return static_cast<int>(socketHandle);
}

std::atomic<uint64_t> g_localListenerCounter = 1;
std::mutex g_localListenerPathsMutex;
std::map<SocketHandle, std::string> g_localListenerPaths;

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

bool createLocalListenerSocket(SocketHandle& listenerSocket, std::string& endpoint, int backlog) {
    listenerSocket = invalidSocket();

    const int socketHandle = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketHandle < 0) {
        return false;
    }

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    endpoint = "/tmp/rsp-local-" + std::to_string(getpid()) + "-" + std::to_string(g_localListenerCounter.fetch_add(1));
    if (endpoint.size() >= sizeof(address.sun_path)) {
        close(socketHandle);
        return false;
    }

#ifdef __APPLE__
    address.sun_len = static_cast<uint8_t>(sizeof(sockaddr_un));
#endif
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    unlink(endpoint.c_str());
    const auto addressLength = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + endpoint.size() + 1);
    if (bind(socketHandle, reinterpret_cast<const sockaddr*>(&address), addressLength) != 0 ||
        listen(socketHandle, backlog) != 0) {
        unlink(endpoint.c_str());
        close(socketHandle);
        return false;
    }

    listenerSocket = fromNative(socketHandle);
    {
        std::lock_guard<std::mutex> lock(g_localListenerPathsMutex);
        g_localListenerPaths[listenerSocket] = endpoint;
    }

    return true;
}

SocketHandle connectLocalListenerSocket(const std::string& endpoint) {
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (endpoint.empty() || endpoint.size() >= sizeof(address.sun_path)) {
        return invalidSocket();
    }

    const int socketHandle = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketHandle < 0) {
        return invalidSocket();
    }

#ifdef __APPLE__
    address.sun_len = static_cast<uint8_t>(sizeof(sockaddr_un));
#endif
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    const auto addressLength = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + endpoint.size() + 1);
    if (connect(socketHandle, reinterpret_cast<const sockaddr*>(&address), addressLength) != 0) {
        close(socketHandle);
        return invalidSocket();
    }

    return fromNative(socketHandle);
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

uint16_t getSocketPort(SocketHandle socketHandle) {
    sockaddr_storage address = {};
    socklen_t addressLength = sizeof(address);
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
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return static_cast<int>(send(toNative(socketHandle), data, length, flags));
}

int recvSocket(SocketHandle socketHandle, uint8_t* buffer, uint32_t length) {
    return static_cast<int>(recv(toNative(socketHandle), buffer, length, 0));
}

SocketHandle createUdpSocket(const std::string& bindAddress, uint16_t port) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const char* host = bindAddress.empty() ? nullptr : bindAddress.c_str();
    if (getaddrinfo(host, portString(port).c_str(), &hints, &result) != 0) {
        return invalidSocket();
    }

    SocketHandle boundSocket = invalidSocket();
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
        const int socketHandle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socketHandle < 0) {
            continue;
        }

        const int reuseAddress = 1;
        setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

        if (bind(socketHandle, current->ai_addr, current->ai_addrlen) == 0) {
            boundSocket = fromNative(socketHandle);
            break;
        }

        close(socketHandle);
    }

    freeaddrinfo(result);
    return boundSocket;
}

SocketHandle connectUdp(const std::string& address, uint16_t port) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

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

int sendSocketTo(SocketHandle socketHandle, const uint8_t* data, uint32_t length,
                 const PeerAddress& destination) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return static_cast<int>(
        sendto(toNative(socketHandle), data, length, flags,
               reinterpret_cast<const sockaddr*>(destination.bytes.data()),
               static_cast<socklen_t>(destination.length)));
}

int recvSocketFrom(SocketHandle socketHandle, uint8_t* buffer, uint32_t length,
                   PeerAddress& source) {
    source = {};
    auto srcLen = static_cast<socklen_t>(source.bytes.size());
    const int result = static_cast<int>(
        recvfrom(toNative(socketHandle), buffer, length, 0,
                 reinterpret_cast<sockaddr*>(source.bytes.data()), &srcLen));
    source.length = static_cast<uint32_t>(srcLen);
    return result;
}

std::string peerAddressKey(const PeerAddress& address) {
    const auto* sa = reinterpret_cast<const sockaddr*>(address.bytes.data());
    if (sa->sa_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
        std::string key(6, '\0');
        std::memcpy(key.data(), &sin->sin_addr.s_addr, 4);
        std::memcpy(key.data() + 4, &sin->sin_port, 2);
        return key;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(sa);
        std::string key(18, '\0');
        std::memcpy(key.data(), sin6->sin6_addr.s6_addr, 16);
        std::memcpy(key.data() + 16, &sin6->sin6_port, 2);
        return key;
    }
    return {};
}

std::string peerAddressString(const PeerAddress& address) {
    const auto* sa = reinterpret_cast<const sockaddr*>(address.bytes.data());
    char hostBuf[INET6_ADDRSTRLEN] = {};
    uint16_t port = 0;
    if (sa->sa_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
        inet_ntop(AF_INET, &sin->sin_addr, hostBuf, sizeof(hostBuf));
        port = ntohs(sin->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(sa);
        inet_ntop(AF_INET6, &sin6->sin6_addr, hostBuf, sizeof(hostBuf));
        port = ntohs(sin6->sin6_port);
    } else {
        return {};
    }
    return std::string(hostBuf) + ":" + std::to_string(port);
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
        std::string path;
        {
            std::lock_guard<std::mutex> lock(g_localListenerPathsMutex);
            const auto iterator = g_localListenerPaths.find(socketHandle);
            if (iterator != g_localListenerPaths.end()) {
                path = iterator->second;
                g_localListenerPaths.erase(iterator);
            }
        }

        if (!path.empty()) {
            unlink(path.c_str());
        }

        close(toNative(socketHandle));
    }
}

}  // namespace rsp::os