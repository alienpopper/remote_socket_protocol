#pragma once

#include "common/transport/transport.hpp"
#include "os/os_socket.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rsp::transport {

// Maximum UDP datagram payload we read at once. Stays within IP datagram limits
// while comfortably exceeding typical RSP message sizes.
constexpr uint32_t kUdpMaxDatagramSize = 65507;

// Server-side per-peer connection sharing the listener socket for sends.
// Receives are fed by the UdpTransport demux loop via enqueue().
class UdpServerConnection : public Connection {
public:
    UdpServerConnection(rsp::os::SocketHandle sharedSocket,
                        rsp::os::PeerAddress peerAddress);
    ~UdpServerConnection() override;

    UdpServerConnection(const UdpServerConnection&) = delete;
    UdpServerConnection& operator=(const UdpServerConnection&) = delete;

    // send: delivers datagram to the peer via sendto on the shared socket.
    int send(const rsp::Buffer& data) override;

    // recv: blocks until a datagram is available, then resizes buffer to fit it.
    int recv(rsp::Buffer& buffer) override;

    void close() override;

    // Called by the UdpTransport demux loop to deliver an incoming datagram.
    void enqueue(std::vector<uint8_t> datagram);

private:
    rsp::os::SocketHandle sharedSocket_;
    rsp::os::PeerAddress peerAddress_;

    std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::deque<std::vector<uint8_t>> queue_;
    bool closed_ = false;
};

// Client-side connection wrapping a connected UDP socket.
// send/recv map directly to the OS send/recv without explicit addresses.
class UdpClientConnection : public Connection {
public:
    explicit UdpClientConnection(rsp::os::SocketHandle socketHandle);
    ~UdpClientConnection() override;

    UdpClientConnection(const UdpClientConnection&) = delete;
    UdpClientConnection& operator=(const UdpClientConnection&) = delete;

    int send(const rsp::Buffer& data) override;

    // recv: receives one full datagram into buffer, resizing buffer to fit.
    int recv(rsp::Buffer& buffer) override;

    void close() override;

private:
    mutable std::mutex stateMutex_;
    rsp::os::SocketHandle socketHandle_;
};

// UDP transport implementing ListeningTransport.
//
// Listening side: bind() a single UDP socket, run a demux thread that calls
// recvfrom() and dispatches datagrams to per-peer UdpServerConnection queues.
// First datagram from a new peer fires the new-connection callback.
//
// Client side: connect() creates a connected UDP socket (UdpClientConnection).
//
// Parameters format: "address:port" (e.g. "127.0.0.1:5000" or "0.0.0.0:0").
class UdpTransport : public ListeningTransport {
public:
    UdpTransport();
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    bool listen(const std::string& parameters) override;

    // Returns the port the transport is listening on (0 if not listening).
    uint16_t listenedPort() const;

    ConnectionHandle connect(const std::string& parameters) override;
    ConnectionHandle reconnect() override;
    void stop() override;
    ConnectionHandle connection() const override;

private:
    static bool parseEndpoint(const std::string& parameters,
                               std::string& address, uint16_t& port);
    void stopListening();
    void stopConnection();
    void demuxLoop();

    using PeerKey = std::string;

    mutable std::mutex stateMutex_;
    rsp::os::SocketHandle listenerSocket_;
    bool listening_ = false;
    std::thread demuxThread_;
    std::map<PeerKey, std::weak_ptr<UdpServerConnection>> peers_;

    std::string lastParameters_;
    ConnectionHandle activeConnection_;
};

}  // namespace rsp::transport
