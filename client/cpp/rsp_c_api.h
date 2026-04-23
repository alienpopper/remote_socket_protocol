#pragma once

// Pure C API for the RSP client.
// Safe to include from any C or C++ context — no protobuf, no C++ types.
// Implemented in rsp_c_api.cpp which wraps the C++ RSPClient.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to an RSP connection (client + background run thread).
typedef struct RspBridge* RspBridgeHandle;

// Creates an RSP client, connects to the Resource Manager at |rm_addr|
// (e.g. "localhost:8080"), and starts the background run loop.
// |rs_node_id| is the bsd_sockets Resource Service node ID string.
// Returns NULL on failure.
RspBridgeHandle rsp_bridge_create(const char* rm_addr,
                                   const char* rs_node_id);

// Opens a TCP connection to |host_port| (e.g. "example.com:80") through the
// bsd_sockets RS identified by this handle.
// Returns a raw socket fd (caller must close() it) or -1 on failure.
int rsp_bridge_connect_tcp(RspBridgeHandle handle, const char* host_port);

// Stops the RSP client and releases all resources. The handle is invalid
// after this call.
void rsp_bridge_destroy(RspBridgeHandle handle);

#ifdef __cplusplus
}
#endif
