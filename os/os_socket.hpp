#pragma once

#include <cstdint>
#include <string>

namespace rsp::os {

using SocketHandle = intptr_t;

bool initializeSockets();
void shutdownSockets();

SocketHandle invalidSocket();
bool isValidSocket(SocketHandle socketHandle);

bool createSocketPair(SocketHandle& firstSocket, SocketHandle& secondSocket);

SocketHandle createTcpListener(const std::string& bindAddress, uint16_t port, int backlog);
SocketHandle acceptSocket(SocketHandle listener);
SocketHandle connectTcp(const std::string& address, uint16_t port);

int sendSocket(SocketHandle socketHandle, const uint8_t* data, uint32_t length);
int recvSocket(SocketHandle socketHandle, uint8_t* buffer, uint32_t length);

void closeSocket(SocketHandle socketHandle);

}  // namespace rsp::os