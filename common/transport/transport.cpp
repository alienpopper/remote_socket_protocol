#include "common/transport/transport.hpp"

namespace rsp::transport {

void ListeningTransport::setNewConnectionCallback(NewConnectionCallback callback) {
    std::lock_guard<std::mutex> lock(newConnectionCallbackMutex_);
    newConnectionCallback_ = std::move(callback);
}

void ListeningTransport::notifyNewConnection(const ConnectionHandle& connection) const {
    NewConnectionCallback callback;

    {
        std::lock_guard<std::mutex> lock(newConnectionCallbackMutex_);
        callback = newConnectionCallback_;
    }

    if (callback) {
        callback(connection);
    }
}

}  // namespace rsp::transport