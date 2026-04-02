#pragma once

#include "common/transport/transport.hpp"
#include "os/os_socket.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace rsp::transport {

class TcpConnection : public Connection {
public:
    explicit TcpConnection(rsp::os::SocketHandle socketHandle);
    ~TcpConnection() override;

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    TcpConnection(TcpConnection&& other) noexcept;
    TcpConnection& operator=(TcpConnection&& other) noexcept;

    int send(const rsp::Buffer& data) override;
    int recv(rsp::Buffer& buffer) override;

private:
    rsp::os::SocketHandle socketHandle_;
};

class TcpTransport : public Transport {
public:
    TcpTransport();
    ~TcpTransport() override;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    bool listen(const std::string& parameters) override;
    std::shared_ptr<Connection> connect(const std::string& parameters) override;

private:
    static bool parseEndpoint(const std::string& parameters, std::string& address, uint16_t& port);
    void stopListening();
    void acceptLoop();

    rsp::os::SocketHandle listener_;
    std::atomic<bool> listening_;
    std::thread acceptThread_;
};

}  // namespace rsp::transport