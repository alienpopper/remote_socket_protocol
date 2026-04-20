#pragma once

#include "common/transport/transport.hpp"
#include "os/os_socket.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace rsp::transport {

// Configuration for automatic reconnection retries in TcpTransport.
// When enabled (the default), reconnect() retries with exponential backoff
// instead of failing on the first unsuccessful attempt.
struct ReconnectConfig {
    bool enabled = true;
    int initialIntervalMs = 500;
    int maxIntervalMs = 10000;
    double backoffMultiplier = 2.0;
    int maxAttempts = 0;  // 0 = unlimited
};

class TcpConnection : public Connection {
public:
    explicit TcpConnection(rsp::os::SocketHandle socketHandle);
    ~TcpConnection() override;

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    int send(const rsp::Buffer& data) override;
    int recv(rsp::Buffer& buffer) override;
    void close() override;

private:
    mutable std::mutex sendMutex_;
    mutable std::mutex recvMutex_;
    mutable std::mutex stateMutex_;
    rsp::os::SocketHandle socketHandle_;
};

class TcpTransport : public ListeningTransport {
public:
    TcpTransport();
    ~TcpTransport() override;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    bool listen(const std::string& parameters) override;
    uint16_t listenedPort() const;
    ConnectionHandle connect(const std::string& parameters) override;
    ConnectionHandle reconnect() override;
    void setReconnectConfig(const ReconnectConfig& config);
    void stop() override;
    ConnectionHandle connection() const override;

private:
    static bool parseEndpoint(const std::string& parameters, std::string& address, uint16_t& port);
    ConnectionHandle connectParsed(const std::string& address, uint16_t port);
    void stopListening();
    void stopConnection();
    void acceptLoop();

    mutable std::mutex stateMutex_;
    rsp::os::SocketHandle listener_;
    bool listening_;
    std::thread acceptThread_;
    std::string lastParameters_;
    ConnectionHandle activeConnection_;

    ReconnectConfig reconnectConfig_;
    bool stopping_;
    std::condition_variable stopCondition_;
    std::mutex stopMutex_;
};

}  // namespace rsp::transport