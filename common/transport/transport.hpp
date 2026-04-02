#pragma once

#include "common/base_types.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace rsp::transport {

class Connection;
class Transport;
class ListeningTransport;

using ConnectionHandle = std::shared_ptr<Connection>;
using TransportHandle = std::shared_ptr<Transport>;
using ListeningTransportHandle = std::shared_ptr<ListeningTransport>;

class Connection {
public:
    virtual ~Connection() = default;

    virtual int send(const rsp::Buffer& data) = 0;
    virtual int recv(rsp::Buffer& buffer) = 0;
    virtual void close() = 0;
};

using NewConnectionCallback = std::function<void(const ConnectionHandle& connection)>;

class Transport {
public:
    virtual ~Transport() = default;

    virtual ConnectionHandle connect(const std::string& parameters) = 0;
    virtual ConnectionHandle reconnect() = 0;
    virtual void stop() = 0;
    virtual ConnectionHandle connection() const = 0;
};

class ListeningTransport : public Transport {
public:
    virtual bool listen(const std::string& parameters) = 0;

    void setNewConnectionCallback(NewConnectionCallback callback);

protected:
    void notifyNewConnection(const ConnectionHandle& connection) const;

private:
    mutable std::mutex newConnectionCallbackMutex_;
    NewConnectionCallback newConnectionCallback_;
};

}  // namespace rsp::transport