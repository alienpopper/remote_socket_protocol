// C API wrapper for RSPClient.
// This file is compiled with the RSP build system (not Chromium) and has full
// access to RSP headers including protobuf-generated code.

#include "client/cpp/rsp_c_api.h"

#include "client/cpp/rsp_client.hpp"

#include <chrono>
#include <exception>
#include <set>
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

    // Enable transparent reconnect so the bridge survives RM restarts.
    bridge->client->enableReconnect(*conn_id);

    return bridge;
}

intptr_t rsp_bridge_connect_tcp(RspBridgeHandle handle, const char* host_port) {
    if (!handle || !host_port) {
        return -1;
    }

    try {
        const rsp::NodeID node_id(handle->rs_node_id);
        auto socket = handle->client->connectTCPSocket(
            node_id,
            std::string(host_port),
            /*timeoutMs=*/5000,
            /*retries=*/2,
            /*retryMs=*/1000);

        return socket.has_value() ? static_cast<intptr_t>(*socket) : -1;
    } catch (const std::exception&) {
        return -1;
    } catch (...) {
        return -1;
    }
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

int rsp_bridge_ping(RspBridgeHandle handle, const char* node_id) {
    if (!handle || !node_id) {
        return 0;
    }

    try {
        return handle->client->ping(rsp::NodeID(node_id)) ? 1 : 0;
    } catch (const std::exception&) {
        return 0;
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

extern "C" {

int rsp_list_bsd_sockets_nodes(const char* rm_addr,
                                char*** out_node_ids,
                                int* out_count) {
    if (!rm_addr || !out_node_ids || !out_count) {
        return 0;
    }
    *out_node_ids = nullptr;
    *out_count = 0;

    try {
        auto client = rsp::client::RSPClient::create();
        const std::string transport = std::string("tcp:") + rm_addr;

        auto conn_id = client->connectToResourceManager(transport, "protobuf");
        if (!conn_id.has_value()) {
            return 0;
        }

        // Run the client's message loop on a background thread so RSP
        // responses (auth, resource query reply) can be processed.
        rsp::client::RSPClient* raw = client.get();
        std::thread run_thread([raw]() { raw->run(); });

        // Poll for the RM peer node ID (available after auth completes).
        std::optional<rsp::NodeID> rm_node_id;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!rm_node_id.has_value() &&
               std::chrono::steady_clock::now() < deadline) {
            rm_node_id = client->peerNodeID(*conn_id);
            if (!rm_node_id.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        if (!rm_node_id.has_value()) {
            client.reset();
            run_thread.join();
            return 0;
        }

        // Query the RM for all registered resource services.
        auto result = client->resourceList(*rm_node_id, "", 200);

        client.reset();
        run_thread.join();

        if (!result.has_value() || !result->success) {
            return 1;  // success but no results
        }

        // Filter for bsd_sockets: services that accept ConnectTCPRequest.
        std::vector<std::string> node_ids;
        std::set<std::string> seen;
        for (const auto& svc : result->services) {
            for (const auto& url : svc.acceptedTypeUrls) {
                if (url.find("ConnectTCPRequest") != std::string::npos) {
                    const std::string id = svc.nodeId.toString();
                    if (seen.insert(id).second) {
                        node_ids.push_back(id);
                    }
                    break;
                }
            }
        }

        if (node_ids.empty()) {
            return 1;
        }

        char** arr = new char*[node_ids.size()];
        for (size_t i = 0; i < node_ids.size(); ++i) {
            arr[i] = new char[node_ids[i].size() + 1];
            std::memcpy(arr[i], node_ids[i].c_str(), node_ids[i].size() + 1);
        }
        *out_node_ids = arr;
        *out_count = static_cast<int>(node_ids.size());
        return 1;
    } catch (...) {
        return 0;
    }
}

void rsp_free_node_ids(char** node_ids, int count) {
    if (!node_ids) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        delete[] node_ids[i];
    }
    delete[] node_ids;
}

}  // extern "C"
