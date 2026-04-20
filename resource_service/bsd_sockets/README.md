# BSD Sockets Resource Service

## What It Is

`rsp_bsd_sockets` is a resource service that exposes ordinary TCP client and TCP listening behavior over RSP.

At a high level, it lets another RSP node ask this service to:

- open an outbound TCP connection to a target host and port
- open a listening TCP socket on the service host
- accept inbound connections on that listening socket
- exchange stream data over the resulting socket
- close sockets when done

The service registers itself with the resource manager and advertises both:

- legacy `ResourceRecord` entries for `tcp_connect` and `tcp_listen`
- a runtime `ServiceSchema` for `bsd_sockets.proto`

The runtime schema is the long-term discovery path.

## Theory Of Use

This service is useful when the machine running the resource service has network reachability that the requesting node does not.

Typical examples:

- a client can reach the resource manager but cannot directly reach a private TCP endpoint
- a service host can open outbound TCP connections on behalf of the client
- a service host can also bind and accept TCP listeners on behalf of the client

The mental model is:

1. The client discovers or already knows the node ID of the BSD sockets resource service.
2. The client sends `ConnectTCPRequest` or `ListenTCPRequest` to that node.
3. The resource service creates or manages the real OS socket locally.
4. The client and service exchange `StreamSend`, `StreamRecv`, and `StreamClose` messages against a `stream_id`.

This means the service host is the machine that actually touches the network.

## Messages And Capabilities

The service schema advertises support for:

- `type.rsp/rsp.proto.ConnectTCPRequest`
- `type.rsp/rsp.proto.ListenTCPRequest`
- `type.rsp/rsp.proto.AcceptTCP`
- `type.rsp/rsp.proto.StreamSend`
- `type.rsp/rsp.proto.StreamRecv`
- `type.rsp/rsp.proto.StreamClose`

Message definitions live in [bsd_sockets.proto](bsd_sockets.proto).

## How Socket Ownership Works

By default, created sockets are owned by the requester node ID that created them.

That means:

- another node cannot send or receive on the socket
- another node cannot close the socket
- another node cannot accept on a listening socket

The ownership checks are relaxed only when the relevant sharing flag is enabled.

The main sharing controls are:

- `share_socket`
- `share_listening_socket`
- `share_child_sockets`

If you do not need cross-node sharing, leave these disabled.

## Connect Flow

Use `ConnectTCPRequest` when you want the service host to make an outbound TCP connection.

Important request fields:

- `host_port`: target address such as `127.0.0.1:8080`
- `stream_id`: caller-chosen socket identifier
- `retries`: retry count, capped at 5 by the implementation
- `retry_ms`: retry delay, capped at 5000 ms by the implementation
- `async_data`: if true, the service pushes `STREAM_DATA` replies asynchronously
- `share_socket`: if true, other node IDs may use the socket

Important constraints:

- `stream_id` is required
- `host_port` is required
- `share_socket` cannot be combined with `async_data`
- `share_socket` cannot be combined with `use_socket`

If the connect succeeds, the service returns `SUCCESS` for that stream ID. After that, data moves through `StreamSend`, `StreamRecv`, and `StreamClose`.

## Listen And Accept Flow

Use `ListenTCPRequest` when you want the service host to bind a local TCP listener.

Important request fields:

- `host_port`: bind address such as `0.0.0.0:9000` or `127.0.0.1:0`
- `stream_id`: caller-chosen listening socket identifier
- `async_accept`: if true, accepted children are pushed back as `NEW_CONNECTION`
- `share_listening_socket`: allows other node IDs to use the listening socket
- `share_child_sockets`: async-accept children may be used by other node IDs
- `children_use_socket`: carried in the API for future behavior
- `children_async_data`: async-accept children automatically use async reads

Important constraints:

- `stream_id` is required
- `host_port` is required
- `share_child_sockets` requires `async_accept`
- `children_use_socket` requires `async_accept`
- `children_async_data` requires `async_accept`

There are two accept models.

### Synchronous Accept

If `async_accept` is false:

- the service queues inbound accepted connections locally
- the caller must send `AcceptTCP`
- the reply includes `new_stream_id` for the accepted child socket

### Asynchronous Accept

If `async_accept` is true:

- the service allocates a child stream automatically
- it sends `NEW_CONNECTION` back to the requester
- the child socket can optionally start in async-data mode
- calling `AcceptTCP` in this mode returns `ASYNC_STREAM`

## Stream Semantics

After a socket exists, the client uses the standard stream messages.

### `StreamSend`

- writes raw bytes to the socket
- returns `SUCCESS` or `STREAM_ERROR`

### `StreamRecv`

- valid only when `async_data` is false for that socket
- returns `STREAM_DATA`, `STREAM_CLOSED`, or `STREAM_ERROR`
- default receive size is 4096 bytes if `max_bytes` is omitted or zero

### `StreamClose`

- closes either a connected socket or a listening socket
- enforces ownership rules unless sharing is enabled

## Async Data Mode

If a socket is created with `async_data = true`:

- a background read thread is started on the service host
- inbound bytes are sent back as `STREAM_DATA`
- EOF becomes `STREAM_CLOSED`
- read errors become `STREAM_ERROR`
- explicit `StreamRecv` requests return `ASYNC_STREAM`

This mode is usually the right fit for bridging to native sockets or event-driven client code.

## Discovery

The service advertises:

- reachable non-local source addresses for outbound TCP connects
- reachable non-local listen addresses for TCP listeners
- an unrestricted port range of `0-0` in the listen advertisement
- the runtime service schema `bsd_sockets.proto`

With the current schema-first query path, a useful discovery query is:

```text
service.proto_file_name = "bsd_sockets.proto"
```

or:

```text
service.accepted_type_urls HAS "type.rsp/rsp.proto.ConnectTCPRequest"
```

## Building

Build only this service with:

```bash
make build/bin/rsp_bsd_sockets
```

The resulting binary is:

```text
build/bin/rsp_bsd_sockets
```

## Configuration

`rsp_bsd_sockets` requires one or more JSON config files passed with `--config`.

Example:

```bash
build/bin/rsp_bsd_sockets --config /path/to/rsp_bsd_sockets.json
```

Multiple config files are allowed and later files override earlier keys.

### Required Fields

- `rm_servers`: array of `host:port` strings

Only the first element is currently used. The binary converts it to `tcp:<host:port>` for the resource manager connection.

### Optional Fields

- `keypair`: `[
  "<public_key_path>",
  "<private_key_path>"
]`

If no keypair is provided, the service generates a fresh keypair on startup.

### Example Config

```json
{
  "rm_servers": ["127.0.0.1:7000"],
  "keypair": [
    "/path/to/resource_service.pub.pem",
    "/path/to/resource_service.pem"
  ]
}
```

## Startup Behavior

On startup the service:

1. loads and merges config files
2. loads or generates a keypair
3. connects to the resource manager using ASCII handshake encoding
4. publishes its advertisement and schema
5. starts serving requests until terminated

The binary prints a successful connection message such as:

```text
rsp_bsd_sockets: connected to tcp:127.0.0.1:7000 using encoding ascii_handshake
```

## Client-Side Use

The C++ client already exposes helpers for this service:

- `connectTCP`
- `listenTCP`
- `streamSend`
- `streamRecv`
- `streamClose`

The practical usage pattern is:

1. discover the service node with a resource query, or configure the node ID out of band
2. call `connectTCP(...)` or `listenTCP(...)`
3. use stream operations against the returned stream ID

## Operational Notes

- outbound connect requests are handled by a worker queue with 8 workers
- async sockets spawn background read threads
- socket IDs must be unique across both connected and listening sockets
- listening on `:0` delegates port selection to the OS
- the service host's local firewall and routing still control what is actually reachable

## When To Use This Service

Use `rsp_bsd_sockets` when you want general TCP transport over RSP.

Do not use it when you want a protocol-specific adapter with its own setup and policy. In that case, prefer a dedicated service such as `rsp_sshd`.