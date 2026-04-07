#pragma once

#include "common/transport/transport.hpp"
#include "os/os_socket.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace rsp::transport {

class MemoryConnection : public Connection {
public:
    explicit MemoryConnection(rsp::os::SocketHandle socketHandle);
    ~MemoryConnection() override;

    MemoryConnection(const MemoryConnection&) = delete;
    MemoryConnection& operator=(const MemoryConnection&) = delete;

    int send(const rsp::Buffer& data) override;
    int recv(rsp::Buffer& buffer) override;
    void close() override;

private:
    mutable std::mutex sendMutex_;
    mutable std::mutex recvMutex_;
    mutable std::mutex stateMutex_;
    rsp::os::SocketHandle socketHandle_;
};

// In-process transport using OS socket pairs (socketpair on POSIX, loopback TCP pair on Windows).
// Both endpoints communicate via byte-stream socket FDs with no network stack involvement.
// Usage: listening side calls listen("channel-name"); connecting side calls connect("channel-name").
// The same handshake, authentication, and encoding pipeline runs as with TcpTransport.
class MemoryTransport : public ListeningTransport {
public:
    MemoryTransport();
    ~MemoryTransport() override;

    MemoryTransport(const MemoryTransport&) = delete;
    MemoryTransport& operator=(const MemoryTransport&) = delete;

    bool listen(const std::string& name) override;
    ConnectionHandle connect(const std::string& name) override;
    ConnectionHandle reconnect() override;
    void stop() override;
    ConnectionHandle connection() const override;

private:
    void stopListening();
    void stopConnection();

    mutable std::mutex stateMutex_;
    bool listening_ = false;
    std::string registeredName_;
    std::string lastConnectName_;
    ConnectionHandle activeConnection_;
};

}  // namespace rsp::transport
