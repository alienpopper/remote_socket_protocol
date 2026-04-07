#include "common/transport/transport_memory.hpp"

#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace rsp::transport {

namespace {

std::mutex g_registryMutex;
std::map<std::string, MemoryTransport*> g_registry;

bool registerListener(const std::string& name, MemoryTransport* transport) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (g_registry.count(name) > 0) {
        return false;
    }
    g_registry[name] = transport;
    return true;
}

void unregisterListener(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    g_registry.erase(name);
}

MemoryTransport* findListener(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    const auto it = g_registry.find(name);
    if (it == g_registry.end()) {
        return nullptr;
    }
    return it->second;
}

}  // namespace

MemoryConnection::MemoryConnection(rsp::os::SocketHandle socketHandle) : socketHandle_(socketHandle) {
}

MemoryConnection::~MemoryConnection() {
    close();
}

int MemoryConnection::send(const rsp::Buffer& data) {
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

int MemoryConnection::recv(rsp::Buffer& buffer) {
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

void MemoryConnection::close() {
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

MemoryTransport::MemoryTransport() {
    if (!rsp::os::initializeSockets()) {
        throw std::runtime_error("failed to initialize socket subsystem");
    }
}

MemoryTransport::~MemoryTransport() {
    stop();
    rsp::os::shutdownSockets();
}

bool MemoryTransport::listen(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (listening_) {
            return false;
        }
    }

    if (!registerListener(name, this)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    registeredName_ = name;
    listening_ = true;
    return true;
}

ConnectionHandle MemoryTransport::connect(const std::string& name) {
    MemoryTransport* const listener = findListener(name);
    if (listener == nullptr) {
        return nullptr;
    }

    rsp::os::SocketHandle clientSocket = rsp::os::invalidSocket();
    rsp::os::SocketHandle serverSocket = rsp::os::invalidSocket();
    if (!rsp::os::createSocketPair(clientSocket, serverSocket)) {
        return nullptr;
    }

    // Hand the server-side socket to the listening transport's new-connection callback.
    listener->notifyNewConnection(std::make_shared<MemoryConnection>(serverSocket));

    auto clientConnection = std::make_shared<MemoryConnection>(clientSocket);

    ConnectionHandle previousConnection;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastConnectName_ = name;
        previousConnection = activeConnection_;
        activeConnection_ = clientConnection;
    }

    if (previousConnection != nullptr && previousConnection != clientConnection) {
        previousConnection->close();
    }

    return clientConnection;
}

ConnectionHandle MemoryTransport::reconnect() {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        name = lastConnectName_;
    }

    if (name.empty()) {
        return nullptr;
    }

    return connect(name);
}

void MemoryTransport::stop() {
    stopListening();
    stopConnection();
}

ConnectionHandle MemoryTransport::connection() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return activeConnection_;
}

void MemoryTransport::stopListening() {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!listening_) {
            return;
        }
        listening_ = false;
        name = registeredName_;
        registeredName_.clear();
    }

    if (!name.empty()) {
        unregisterListener(name);
    }
}

void MemoryTransport::stopConnection() {
    ConnectionHandle activeConnection;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        activeConnection = std::move(activeConnection_);
    }

    if (activeConnection != nullptr) {
        activeConnection->close();
    }
}

}  // namespace rsp::transport
