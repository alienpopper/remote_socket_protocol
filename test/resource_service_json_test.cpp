#define RSPCLIENT_STATIC

#include "resource_service/resource_service.hpp"

#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/transport/transport_memory.hpp"
#include "resource_manager/resource_manager.hpp"
#include "os/os_socket.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class TestResourceManager : public rsp::resource_manager::ResourceManager {
public:
    using rsp::resource_manager::ResourceManager::ResourceManager;

    rsp::NodeID nodeId() const {
        return keyPair().nodeID();
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool waitForCondition(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return condition();
}

std::string normalizeAddress(const rsp::os::IPAddress& address) {
    if (address.family == rsp::os::IPAddressFamily::IPv4) {
        return std::string("4:") + std::to_string(address.ipv4);
    }

    return std::string("6:") + std::string(reinterpret_cast<const char*>(address.ipv6.data()), address.ipv6.size());
}

std::string normalizeAddress(const rsp::proto::Address& address) {
    if (!address.ipv6().empty()) {
        return std::string("6:") + address.ipv6();
    }

    return std::string("4:") + std::to_string(address.ipv4());
}

std::set<std::string> expectedAdvertisedAddresses() {
    std::set<std::string> addresses;
    for (const auto& address : rsp::os::listNonLocalAddresses()) {
        addresses.insert(normalizeAddress(address));
    }

    return addresses;
}

void testResourceServiceConnectsToResourceManagerUsingJsonEncoding() {
    auto serverTransport = std::make_shared<rsp::transport::MemoryTransport>();
    TestResourceManager resourceManager({serverTransport});

    rsp::KeyPair resourceServiceKeyPair = rsp::KeyPair::generateP256();
    const rsp::NodeID resourceServiceNodeId = resourceServiceKeyPair.nodeID();

    std::promise<rsp::encoding::EncodingHandle> handshakePromise;
    auto handshakeFuture = handshakePromise.get_future();
    resourceManager.setNewEncodingCallback([&handshakePromise](const rsp::encoding::EncodingHandle& encoding) {
        try {
            handshakePromise.set_value(encoding);
        } catch (...) {
            handshakePromise.set_exception(std::current_exception());
        }
    });

    require(serverTransport->listen("rm-json-test"), "resource manager listener should start for JSON test");
    const std::string transportSpec = "memory:rm-json-test";

    auto resourceService = rsp::resource_service::ResourceService::create(std::move(resourceServiceKeyPair));
    const auto connectionId = resourceService->connectToResourceManager(transportSpec, rsp::message_queue::kJsonHandshakeEncoding);

    require(resourceService->hasConnection(connectionId), "resource service should track the JSON RM connection");
    require(handshakeFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "resource manager handshake pipeline should complete for JSON encoding");

    const auto serverEncoding = handshakeFuture.get();
    require(serverEncoding != nullptr, "resource manager should activate a JSON encoding for the resource service");
    require(resourceManager.activeEncodingCount() == 1,
            "resource manager should expose one authenticated JSON encoding");

    const auto resourceServicePeerNodeId = resourceService->peerNodeID(connectionId);
    require(resourceServicePeerNodeId.has_value(),
            "resource service should learn the resource manager node id during JSON authentication");
    require(resourceServicePeerNodeId.value() == resourceManager.nodeId(),
            "resource service should store the resource manager node id for the JSON connection");

    const auto serverPeerNodeId = serverEncoding->peerNodeID();
    require(serverPeerNodeId.has_value(),
            "resource manager encoding should learn the resource service node id during JSON authentication");
    require(serverPeerNodeId.value() == resourceServiceNodeId,
            "resource manager encoding should store the resource service node id for the JSON connection");

    require(waitForCondition([&resourceManager, &resourceServiceNodeId]() {
                return resourceManager.hasResourceAdvertisement(resourceServiceNodeId);
            }),
            "resource manager should store a resource advertisement received over JSON");

    const auto advertisement = resourceManager.resourceAdvertisement(resourceServiceNodeId);
    require(advertisement.has_value(), "resource manager should return the stored JSON resource advertisement");
    require(advertisement->records_size() == 2,
            "JSON resource advertisement should include TCP connect and TCP listen records");

    const auto expectedAddresses = expectedAdvertisedAddresses();
    bool sawTcpConnect = false;
    bool sawTcpListen = false;
    for (const auto& record : advertisement->records()) {
        if (record.has_tcp_connect()) {
            sawTcpConnect = true;
            std::set<std::string> advertisedAddresses;
            for (const auto& address : record.tcp_connect().source_addresses()) {
                advertisedAddresses.insert(normalizeAddress(address));
            }
            require(advertisedAddresses == expectedAddresses,
                    "JSON TCP connect advertisement should preserve all non-local addresses");
        }

        if (record.has_tcp_listen()) {
            sawTcpListen = true;
            std::set<std::string> advertisedAddresses;
            for (const auto& address : record.tcp_listen().listen_address()) {
                advertisedAddresses.insert(normalizeAddress(address));
            }
            require(advertisedAddresses == expectedAddresses,
                    "JSON TCP listen advertisement should preserve all non-local addresses");
            require(record.tcp_listen().has_allowed_range() &&
                        record.tcp_listen().allowed_range().start_port() == 0 &&
                        record.tcp_listen().allowed_range().end_port() == 0,
                    "JSON TCP listen advertisement should preserve the unrestricted port range");
        }
    }

    require(sawTcpConnect, "JSON resource advertisement should include a TCP connect record");
    require(sawTcpListen, "JSON resource advertisement should include a TCP listen record");

    require(resourceService->removeConnection(connectionId), "resource service should remove the JSON RM connection");
    serverTransport->stop();
}

}  // namespace

int main() {
    try {
        testResourceServiceConnectsToResourceManagerUsingJsonEncoding();
        std::cout << "resource_service_json_test passed" << std::endl;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "resource_service_json_test failed: " << exception.what() << std::endl;
        return 1;
    }
}