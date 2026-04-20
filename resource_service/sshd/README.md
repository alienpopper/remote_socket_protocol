# SSHD Resource Service

## What It Is

`rsp_sshd` is a resource service that exposes SSH server sessions over RSP.

It is a specialized adapter built on top of the BSD sockets resource-service machinery, but instead of opening arbitrary TCP sockets it spawns `sshd -i` for each incoming RSP connection request and bridges that child process over a socketpair.

In practice, this gives you an SSH server endpoint reachable through the resource manager even when the client cannot directly reach the target host over TCP.

## Theory Of Use

The service host is the machine that can run `sshd` locally. A client does not connect to TCP port 22 directly. Instead:

1. the client connects to the resource manager
2. the client discovers or already knows the node ID of `rsp_sshd`
3. the client sends `ConnectSshd` to that node
4. `rsp_sshd` creates a socketpair and forks `sshd -i`
5. one end of the socketpair is attached to the child process as stdin/stdout
6. the other end is exposed back to the RSP client as a stream
7. the client exchanges SSH bytes through standard stream messages

The resulting SSH protocol still runs end-to-end between the local SSH client and the remote `sshd`. RSP only carries the byte stream.

## What The Service Supports

The runtime schema advertises support for:

- `type.rsp/rsp.proto.ConnectSshd`
- `type.rsp/rsp.proto.StreamSend`
- `type.rsp/rsp.proto.StreamRecv`
- `type.rsp/rsp.proto.StreamClose`

The request type definition lives in [sshd.proto](sshd.proto).

## What It Does Not Support

`rsp_sshd` is intentionally not a generic BSD sockets endpoint.

It rejects these BSD-sockets-specific requests:

- `ConnectTCPRequest`
- `ListenTCPRequest`
- `AcceptTCP`

If you need general outbound or listening TCP behavior, use `rsp_bsd_sockets` instead.

## Connect Model

The service-specific request is `ConnectSshd`.

Important fields:

- `stream_id`: caller-chosen socket identifier
- `async_data`: if true, data is pushed asynchronously
- `share_socket`: allows other node IDs to use the stream

Important constraints enforced by the implementation:

- `stream_id` is required
- `share_socket` cannot be combined with `async_data`
- `share_socket` cannot be combined with `use_socket`

Once the service accepts the request, the stream behaves like any other RSP stream:

- `StreamSend` writes bytes into the SSH session
- `StreamRecv` reads bytes when the stream is not async
- `StreamClose` tears the session down

## Runtime Model

For each `ConnectSshd` request, the service:

1. creates a Unix `socketpair`
2. forks the current process
3. in the child, dup2s the socket onto stdin and stdout
4. execs `sshd -i`
5. in the parent, wraps the other socket end as an RSP connection object
6. registers that connection under the requested stream ID

This means each incoming SSH session has a dedicated `sshd -i` child.

## Discovery

With the schema-first query path, the cleanest discovery query is:

```text
service.proto_file_name = "sshd.proto"
```

or:

```text
service.accepted_type_urls HAS "type.rsp/rsp.proto.ConnectSshd"
```

The service also still advertises a legacy `sshd` resource record for compatibility.

## Building

Build only this service with:

```bash
make rsp-sshd
```

The resulting binary is:

```text
build/bin/rsp_sshd
```

## Configuration

The binary accepts a single optional config path argument.

If omitted, it defaults to:

```text
/etc/rsp-sshd/rsp_sshd.conf.json
```

Example:

```bash
build/bin/rsp_sshd /etc/rsp-sshd/rsp_sshd.conf.json
```

### Required Fields

- `rsp_transport`: resource manager transport such as `tcp:rm-host:7000`

### Optional Fields

- `sshd_path`: path to the `sshd` binary, default `/usr/sbin/sshd`
- `sshd_config`: path to an `sshd_config` file
- `sshd_debug`: if true, runs `sshd -d -i`

### Example Config

```json
{
  "rsp_transport": "tcp:127.0.0.1:7000",
  "sshd_path": "/usr/sbin/sshd",
  "sshd_config": "/etc/ssh/sshd_config",
  "sshd_debug": false
}
```

## Setup Requirements

For this service to work reliably, the host running `rsp_sshd` needs:

- a working `sshd` binary
- host keys that `sshd -i` can read
- an `sshd_config` compatible with inetd mode
- a reachable resource manager transport

### Host Key Access

This is the most common setup issue.

If the normal system host keys are readable only by root and `rsp_sshd` runs unprivileged, `sshd -i` may fail. In that case, generate a separate key set and point `sshd_config` at it.

Example:

```bash
mkdir -p /etc/rsp-sshd/host-keys
ssh-keygen -t ed25519 -f /etc/rsp-sshd/host-keys/ssh_host_ed25519_key -N ""
ssh-keygen -t rsa -f /etc/rsp-sshd/host-keys/ssh_host_rsa_key -N ""
```

Then use an `sshd_config` containing matching `HostKey` directives.

## Startup Behavior

On startup the service:

1. loads the JSON config
2. installs signal handlers for `SIGTERM`, `SIGINT`, and `SIGCHLD`
3. ignores `SIGPIPE`
4. creates the resource service
5. connects to the resource manager using ASCII handshake encoding
6. logs its node ID
7. stays alive until termination is requested

Expected startup log lines look like:

```text
[rsp-sshd] Connected to RSP transport: tcp:127.0.0.1:7000
[rsp-sshd] Node ID: <uuid>
[rsp-sshd] Registered as ResourceService — ready to accept SSH connections
```

That node ID is what clients or discovery tooling need to target.

## Client-Side Use

The C++ client already exposes helpers for this service:

- `connectSshdEx(...)`
- `connectSshd(...)`
- `connectSshdSocket(...)`

The typical sequence is:

1. discover the `rsp_sshd` node ID or configure it out of band
2. call `connectSshd(...)` or `connectSshdSocket(...)`
3. bridge an SSH client or custom byte stream over the returned stream

The repo also includes `rsp_ssh`, which is a dedicated SSH `ProxyCommand` client for this service.

## Operational Notes

- each new RSP SSH session spawns a new `sshd -i` child process
- child exit is reaped through `SIGCHLD`
- the parent closes inherited file descriptors before `exec`
- shutdown sends `SIGTERM` to the child connection wrapper on close
- `sshd_debug = true` is useful for troubleshooting but not ideal for routine production use

## Theory Of Security

This service does not replace SSH authentication. It only changes the transport path.

Security still depends on:

- normal SSH authentication and authorization on the remote host
- RSP-level routing, identity, and endorsement policy between the client and the service
- securing the resource manager and any endorsement service involved in the deployment

In other words, `rsp_sshd` is an SSH transport adapter, not an SSH policy replacement.

## Recommended Deployment Pattern

Use `rsp_sshd` when:

- the server host can reach the resource manager
- the client can reach the resource manager
- direct network connectivity between client and server is unavailable or undesirable

If you want a full example deployment with `rsp_ssh`, resource manager, endorsement service, config files, and service units, see:

[integration/openssh/modification/README.md](../../integration/openssh/modification/README.md)