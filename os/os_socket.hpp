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

int sendSocket(SocketHandle socketHandle, const uint8_t* data, uint32_t length);
int recvSocket(SocketHandle socketHandle, uint8_t* buffer, uint32_t length);

std::vector<IPAddress> listNonLocalAddresses();

void closeSocket(SocketHandle socketHandle);

}  // namespace rsp::os