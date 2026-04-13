# remote_socket_protocol

RSP (Remote Socket Protocol) is a transport-agnostic socket relay infrastructure
designed for debugging, pentesting, and hardware testing across isolated networks.
It replaces ad-hoc tools like SSH tunnels, VPNs, and netcat with a stable,
authenticated, routable socket layer.

## Core Concepts

| Component | Role |
|---|---|
| **RM** (Resource Manager) | Central router; all nodes connect to it |
| **RS** (Resource Service) | Provides socket access (e.g. SSH, HTTP) to clients |
| **CS** (Client Service) | Consumes remote sockets (client-side applications) |
| **ES** (Endorsement Service) | Issues cryptographic endorsements controlling access |
| **NS** (Name Service) | Maps human-readable names to Node IDs |

See [`architecture.md`](architecture.md) for the full design.

## Integrations

### OpenSSH over RSP

Run `ssh` through RSP with no direct network path between client and server.

- **`rsp_sshd`**: registers as a ResourceService; spawns `sshd -i` per incoming connection.
- **`rsp_ssh`**: SSH `ProxyCommand` that connects via RSP.

```bash
ssh -o "ProxyCommand=rsp_ssh ~/.rsp-ssh/rsp_ssh.conf.json" user@host
```

See [`integration/openssh/modification/README.md`](integration/openssh/modification/README.md)
for full build, deployment, and configuration instructions.

### Node.js Express over RSP

Serve an Express HTTP app over RSP sockets.

See [`integration/nodejs_express/modification/README.md`](integration/nodejs_express/modification/README.md).

## Building

Dependencies: cmake 3.22+, protobuf, boringssl (fetched via `third_party/`).

```bash
make            # build everything
make rsp-sshd   # build/bin/rsp_sshd only
make rsp-ssh    # build/bin/rsp_ssh only
```

## License

Add a license before publishing if needed.
