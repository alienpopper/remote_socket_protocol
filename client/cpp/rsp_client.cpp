#include "client/cpp/rsp_client.hpp"

#include "common/ascii_handshake.hpp"
#include "common/encoding/protobuf/protobuf_encoding.hpp"
#include "common/transport/transport_tcp.hpp"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rsp::client {

RSPClient::Ptr RSPClient::create() {
    return Ptr(new RSPClient(KeyPair::generateP256()));
}

RSPClient::Ptr RSPClient::create(KeyPair keyPair) {
    return Ptr(new RSPClient(std::move(keyPair)));
}

RSPClient::RSPClient(KeyPair keyPair)
    : rsp::RSPNode(std::move(keyPair)), incomingMessages_(std::make_shared<rsp::BufferedMessageQueue>()) {
}

int RSPClient::run() const {
    return 0;
}

RSPClient::TransportID RSPClient::createTcpTransport() {
    return addTransport(std::make_shared<rsp::transport::TcpTransport>());
}

RSPClient::TransportID RSPClient::addTransport(const rsp::transport::TransportHandle& transport) {
    if (transport == nullptr) {
        throw std::invalid_argument("transport must not be null");
    }

    const TransportID transportId;

    std::lock_guard<std::mutex> lock(transportsMutex_);
    transports_.emplace(transportId, transport);
    return transportId;
}

rsp::transport::ConnectionHandle RSPClient::connect(TransportID transportId, const std::string& parameters) const {
    const rsp::transport::TransportHandle selectedTransport = transport(transportId);
    if (selectedTransport == nullptr) {
        return nullptr;
    }

    const rsp::transport::ConnectionHandle connection = selectedTransport->connect(parameters);
    if (connection == nullptr) {
        return nullptr;
    }

    if (!performAsciiHandshake(connection)) {
        selectedTransport->stop();
        return nullptr;
    }

    rsp::encoding::EncodingHandle previousEncoding;
    const auto newEncoding = std::make_shared<rsp::encoding::protobuf::ProtobufEncoding>(connection, incomingMessages_);
    if (!newEncoding->start()) {
        selectedTransport->stop();
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(transportsMutex_);
        const auto iterator = encodings_.find(transportId);
        if (iterator != encodings_.end()) {
            previousEncoding = iterator->second;
            iterator->second = newEncoding;
        } else {
            encodings_.emplace(transportId, newEncoding);
        }
    }

    if (previousEncoding != nullptr) {
        previousEncoding->stop();
    }

    return connection;
}

bool RSPClient::send(TransportID transportId, const rsp::proto::RSPMessage& message) const {
    const auto selectedEncoding = encoding(transportId);
    if (selectedEncoding == nullptr) {
        return false;
    }

    const auto outgoingMessages = selectedEncoding->outgoingMessages();
    return outgoingMessages != nullptr && outgoingMessages->push(message);
}

bool RSPClient::tryDequeueMessage(rsp::proto::RSPMessage& message) const {
    return incomingMessages_ != nullptr && incomingMessages_->tryPop(message);
}

std::size_t RSPClient::pendingMessageCount() const {
    return incomingMessages_ == nullptr ? 0 : incomingMessages_->size();
}

bool RSPClient::hasTransports() const {
    std::lock_guard<std::mutex> lock(transportsMutex_);
    return !transports_.empty();
}

bool RSPClient::hasTransport(TransportID transportId) const {
    std::lock_guard<std::mutex> lock(transportsMutex_);
    return transports_.find(transportId) != transports_.end();
}

std::size_t RSPClient::transportCount() const {
    std::lock_guard<std::mutex> lock(transportsMutex_);
    return transports_.size();
}

std::vector<RSPClient::TransportID> RSPClient::transportIds() const {
    std::vector<TransportID> transportIds;

    std::lock_guard<std::mutex> lock(transportsMutex_);
    transportIds.reserve(transports_.size());
    for (const auto& [transportId, _] : transports_) {
        transportIds.push_back(transportId);
    }

    return transportIds;
}

bool RSPClient::removeTransport(TransportID transportId) {
    rsp::transport::TransportHandle removedTransport;
    rsp::encoding::EncodingHandle removedEncoding;

    {
        std::lock_guard<std::mutex> lock(transportsMutex_);
        const auto iterator = transports_.find(transportId);
        if (iterator == transports_.end()) {
            return false;
        }

        removedTransport = iterator->second;
        transports_.erase(iterator);

        const auto encodingIterator = encodings_.find(transportId);
        if (encodingIterator != encodings_.end()) {
            removedEncoding = encodingIterator->second;
            encodings_.erase(encodingIterator);
        }
    }

    if (removedEncoding != nullptr) {
        removedEncoding->stop();
    }

    removedTransport->stop();
    return true;
}

rsp::transport::TransportHandle RSPClient::transport(TransportID transportId) const {
    std::lock_guard<std::mutex> lock(transportsMutex_);
    const auto iterator = transports_.find(transportId);
    if (iterator == transports_.end()) {
        return nullptr;
    }

    return iterator->second;
}

bool RSPClient::performAsciiHandshake(const rsp::transport::ConnectionHandle& connection) const {
    return rsp::ascii_handshake::performClientHandshake(connection);
}

rsp::encoding::EncodingHandle RSPClient::encoding(TransportID transportId) const {
    std::lock_guard<std::mutex> lock(transportsMutex_);
    const auto iterator = encodings_.find(transportId);
    if (iterator == encodings_.end()) {
        return nullptr;
    }

    return iterator->second;
}

}  // namespace rsp::client