#include "client/cpp/rsp_client.hpp"

#include "common/ascii_handshake.hpp"
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

RSPClient::RSPClient(KeyPair keyPair) : rsp::RSPNode(std::move(keyPair)) {
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

    return connection;
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

    {
        std::lock_guard<std::mutex> lock(transportsMutex_);
        const auto iterator = transports_.find(transportId);
        if (iterator == transports_.end()) {
            return false;
        }

        removedTransport = iterator->second;
        transports_.erase(iterator);
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

}  // namespace rsp::client