#include "common/transport/transport.hpp"

#include <algorithm>
#include <stdexcept>

namespace rsp::transport {

bool Connection::readExact(uint8_t* destination, uint32_t length) {
    uint32_t totalRead = 0;
    while (totalRead < length) {
        rsp::Buffer chunk(length - totalRead);
        const int bytesRead = recv(chunk);
        if (bytesRead <= 0) {
            return false;
        }

        std::copy_n(chunk.data(), static_cast<uint32_t>(bytesRead), destination + totalRead);
        totalRead += static_cast<uint32_t>(bytesRead);
    }

    return true;
}

bool Connection::sendAll(const uint8_t* data, uint32_t length) {
    uint32_t totalSent = 0;
    while (totalSent < length) {
        rsp::Buffer chunk(data + totalSent, length - totalSent);
        const int bytesSent = send(chunk);
        if (bytesSent <= 0) {
            return false;
        }

        totalSent += static_cast<uint32_t>(bytesSent);
    }

    return true;
}

void Connection::setNegotiatedEncoding(const std::string& encodingName) {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    if (negotiatedEncoding_.has_value() && negotiatedEncoding_.value() != encodingName) {
        throw std::logic_error("connection encoding is already established");
    }

    negotiatedEncoding_ = encodingName;
}

std::optional<std::string> Connection::negotiatedEncoding() const {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    return negotiatedEncoding_;
}

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