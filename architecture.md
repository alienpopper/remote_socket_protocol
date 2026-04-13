# RSP Architecture (As-Built)

This document describes what is implemented in this repository today.

## Implemented Components

- RM (Resource Manager)
- RS (Resource Service)
- ES (Endorsement Service)
- CS-side clients and adapters (C++, Node.js, Python)

## NS Status (As-Designed)

Name Service (NS) remains as-designed and is not implemented yet.

The intended NS role is to map human-readable names to Node IDs and support
operational redirects/failover. Keep all NS behavior in this repository treated
as design intent until implementation lands.

## Current Runtime Topology

Typical integration/test topology in this repository:

- One RM instance accepts edge node connections.
- One ES instance issues endorsements.
- One or more RS/CS nodes connect to RM.
- RS nodes provide socket operations (listen/connect/send/recv/close) to clients.

## Message And Session Model

- Node identity is keypair-based (Node ID derived from key material).
- Sessions start with an ASCII handshake and transition to a negotiated encoding.
- Internal RM processing uses protobuf message structures.
- Node-facing generated bindings are maintained for Node.js and Python.

## Security Model (Implemented Behavior)

- RSP messages are authenticated and integrity protected.
- The transport itself is not confidential; payload confidentiality is expected to
  come from tunneled protocols (for example SSH/TLS).
- Endorsements are used for authorization decisions.

## Implemented Integration Paths

- OpenSSH over RSP
  - `rsp_sshd` runs as an RS and bridges incoming RSP sockets to `sshd -i`.
  - `rsp_ssh` runs as SSH `ProxyCommand` and bridges stdio over RSP.
- Node.js Express over RSP
  - Node app listens through RSP and is validated by integration and stress tests.
- Python HTTP server over RSP
  - Python app listens through RSP and is validated by integration tests.

## Build/Test Signals

The following top-level targets are available and exercised in this repository:

- `make test`
- `make test-nodejs-client`
- `make test-nodejs-client-reconnect`
- `make test-nodejs-express`
- `make test-nodejs-express-stress`
- `make test-python-http-server`

## Scope Notes

- This document intentionally reflects current implementation.
- Future/aspirational behavior should be documented separately or clearly labeled
  as design-only (as done for NS above).
