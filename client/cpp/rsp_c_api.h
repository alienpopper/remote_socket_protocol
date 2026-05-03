#pragma once

// Pure C API for the RSP client.
// Safe to include from any C or C++ context — no protobuf, no C++ types.
// Implemented in rsp_c_api.cpp which wraps the C++ RSPClient.

#include <stdint.h>

#include "client/cpp/rsp_client_export.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to an RSP connection (client + background run thread).
typedef struct RspBridge* RspBridgeHandle;

// Creates an RSP client, connects to the Resource Manager at |rm_addr|
// (e.g. "localhost:8080"), and starts the background run loop.
// |rs_node_id| is the bsd_sockets Resource Service node ID string.
// Returns NULL on failure.
RSPCLIENT_API RspBridgeHandle rsp_bridge_create(const char* rm_addr,
                                                const char* rs_node_id);

// Opens a TCP connection to |host_port| (e.g. "example.com:80") through the
// bsd_sockets RS identified by this handle.
// Returns a raw socket handle (caller must close/closesocket it) or -1 on
// failure.
RSPCLIENT_API intptr_t rsp_bridge_connect_tcp(RspBridgeHandle handle, const char* host_port);

// Opens a plain HTTP byte stream to an httpd Resource Service node. |httpd_node_id|
// is the RSP node ID from the rsp:// authority; |virtual_host| is optional and
// is forwarded in ConnectHttp for vhost routing. TLS is intentionally disabled.
// Returns a raw socket handle (caller must close/closesocket it) or -1 on
// failure.
RSPCLIENT_API intptr_t rsp_bridge_connect_http(RspBridgeHandle handle,
                                               const char* httpd_node_id,
                                               const char* virtual_host);

// Sends an RSP Ping to |node_id| through this RM connection.
// Returns 1 when a reply is received, 0 otherwise.
RSPCLIENT_API int rsp_bridge_ping(RspBridgeHandle handle, const char* node_id);

// Stops the RSP client and releases all resources. The handle is invalid
// after this call.
RSPCLIENT_API void rsp_bridge_destroy(RspBridgeHandle handle);

// Lists bsd_sockets Resource Service node IDs registered with the RM at
// |rm_addr|. On success returns 1 and sets |*out_node_ids| to a newly
// allocated array of |*out_count| null-terminated node ID strings. The array
// must be freed with rsp_free_node_ids(). On failure returns 0.
// Blocks for up to ~8 seconds (connect + query timeout).
RSPCLIENT_API int rsp_list_bsd_sockets_nodes(const char* rm_addr,
                                              char*** out_node_ids,
                                              int* out_count);

// Frees the array returned by rsp_list_bsd_sockets_nodes.
RSPCLIENT_API void rsp_free_node_ids(char** node_ids, int count);

#ifdef __cplusplus
}
#endif
