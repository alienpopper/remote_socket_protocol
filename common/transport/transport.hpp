#pragma once

#include "common/base_types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rsp::transport {

class Connection {
public:
    virtual ~Connection() = default;

    virtual int send(const rsp::Buffer& data) = 0;
    virtual int recv(rsp::Buffer& buffer) = 0;
};

using NewConnectionCallback = std::function<void(const std::shared_ptr<Connection>& connection)>;

class Transport {
public:
    virtual ~Transport() = default;

    virtual bool listen(const std::string& parameters) = 0;
    virtual std::shared_ptr<Connection> connect(const std::string& parameters) = 0;

    void setNewConnectionCallback(NewConnectionCallback callback);

protected:
    void notifyNewConnection(const std::shared_ptr<Connection>& connection) const;

private:
    NewConnectionCallback newConnectionCallback_;
};

}  // namespace rsp::transport