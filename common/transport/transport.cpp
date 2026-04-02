#include "common/transport/transport.hpp"

namespace rsp::transport {

void Transport::setNewConnectionCallback(NewConnectionCallback callback) {
    newConnectionCallback_ = std::move(callback);
}

void Transport::notifyNewConnection(const std::shared_ptr<Connection>& connection) const {
    if (newConnectionCallback_) {
        newConnectionCallback_(connection);
    }
}

}  // namespace rsp::transport