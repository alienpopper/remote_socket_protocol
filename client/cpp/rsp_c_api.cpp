// C API wrapper for RSPClient.
// This file is compiled with the RSP build system (not Chromium) and has full
// access to RSP headers including protobuf-generated code.

#include "client/cpp/rsp_c_api.h"

#include "client/cpp/rsp_client.hpp"

#include <exception>
#include <string>
#include <thread>

struct RspBridge {
    rsp::client::RSPClient::Ptr client;
    std::string rs_node_id;
    std::thread run_thread;
};

extern "C" {

RspBridgeHandle rsp_bridge_create(const char* rm_addr,
                                   const char* rs_node_id) {
    if (!rm_addr || !rs_node_id) {
        return nullptr;
    }

    auto client = rsp::client::RSPClient::create();
    const std::string transport = std::string("tcp:") + rm_addr;

    auto conn_id = client->connectToResourceManager(transport, "protobuf");
    if (!conn_id.has_value()) {
        return nullptr;
    }

    auto* bridge = new RspBridge{};
    bridge->rs_node_id = rs_node_id;
    bridge->client = std::move(client);

    rsp::client::RSPClient* raw = bridge->client.get();
    bridge->run_thread = std::thread([raw]() { raw->run(); });

    return bridge;
}

intptr_t rsp_bridge_connect_tcp(RspBridgeHandle handle, const char* host_port) {
    if (!handle || !host_port) {
        return -1;
    }

    const rsp::NodeID node_id(handle->rs_node_id);
    auto socket = handle->client->connectTCPSocket(
        node_id,
        std::string(host_port),
        /*timeoutMs=*/5000,
        /*retries=*/2,
        /*retryMs=*/1000);

    return socket.has_value() ? static_cast<intptr_t>(*socket) : -1;
}

intptr_t rsp_bridge_connect_http(RspBridgeHandle handle,
                                 const char* httpd_node_id,
                                 const char* virtual_host) {
    if (!handle || !httpd_node_id) {
        return -1;
    }

    try {
        const rsp::NodeID node_id(httpd_node_id);
        auto socket = handle->client->connectHttpSocket(
            node_id,
            /*timeoutMs=*/5000,
            virtual_host == nullptr ? std::string() : std::string(virtual_host));

        return socket.has_value() ? static_cast<intptr_t>(*socket) : -1;
    } catch (const std::exception&) {
        return -1;
    }
}

void rsp_bridge_destroy(RspBridgeHandle handle) {
    if (!handle) {
        return;
    }
    // Releasing the Ptr triggers RSPClient::~RSPClient() which sets stopping_
    // and notifies stateChanged_, causing run() to return in run_thread.
    handle->client.reset();
    if (handle->run_thread.joinable()) {
        handle->run_thread.join();
    }
    delete handle;
}

}  // extern "C"
