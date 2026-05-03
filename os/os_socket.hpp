#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rsp::os {

using SocketHandle = intptr_t;

enum class IPAddressFamily {
	IPv4,
	IPv6,
};

struct IPAddress {
	IPAddressFamily family = IPAddressFamily::IPv4;
	uint32_t ipv4 = 0;
	std::array<uint8_t, 16> ipv6 = {};

	bool operator<(const IPAddress& other) const {
		if (family != other.family) {
			return family < other.family;
		}

		if (family == IPAddressFamily::IPv4) {
			return ipv4 < other.ipv4;
		}

		return ipv6 < other.ipv6;
	}
};

// Opaque storage for a UDP peer address. Sized to hold any sockaddr variant.
struct PeerAddress {
    std::array<uint8_t, 128> bytes = {};
    uint32_t length = 0;
};

bool initializeSockets();
void shutdownSockets();

SocketHandle invalidSocket();
bool isValidSocket(SocketHandle socketHandle);

bool createSocketPair(SocketHandle& firstSocket, SocketHandle& secondSocket);
bool createLocalListenerSocket(SocketHandle& listenerSocket, std::string& endpoint, int backlog = 16);
SocketHandle connectLocalListenerSocket(const std::string& endpoint);

SocketHandle createTcpListener(const std::string& bindAddress, uint16_t port, int backlog);
uint16_t getSocketPort(SocketHandle socketHandle);
SocketHandle acceptSocket(SocketHandle listener);
SocketHandle connectTcp(const std::string& address, uint16_t port);

// Creates and binds a SOCK_DGRAM/IPPROTO_UDP socket. Pass port=0 for ephemeral.
SocketHandle createUdpSocket(const std::string& bindAddress, uint16_t port);

// Creates a connected UDP socket (sets default remote peer via connect()).
SocketHandle connectUdp(const std::string& address, uint16_t port);

// sendto: sends to an explicit destination, used by server-side UDP connections.
int sendSocketTo(SocketHandle socketHandle, const uint8_t* data, uint32_t length,
                 const PeerAddress& destination);

// recvfrom: receives a datagram and returns the sender's address.
int recvSocketFrom(SocketHandle socketHandle, uint8_t* buffer, uint32_t length,
                   PeerAddress& source);

// Returns a compact key string (addr bytes + port) suitable for map lookup.
std::string peerAddressKey(const PeerAddress& address);

// Returns a human-readable "host:port" string for logging/display.
std::string peerAddressString(const PeerAddress& address);

int sendSocket(SocketHandle socketHandle, const uint8_t* data, uint32_t length);
int recvSocket(SocketHandle socketHandle, uint8_t* buffer, uint32_t length);

std::vector<IPAddress> listNonLocalAddresses();

void closeSocket(SocketHandle socketHandle);

}  // namespace rsp::os