# RSP Architecture Primer

This document is a primer for new developers, users, and LLM-based agents.
It explains the structure, intent, and current implementation shape of the
Remote Socket Protocol (RSP) system.

## Why RSP exists

In test and lab environments, teams often need to connect software across
isolated or awkward networks. Traditional ad-hoc tooling such as SSH tunnels,
VPNs, and one-off forwarding scripts works, but it is brittle and hard to
observe.

RSP provides a message-authenticated routing fabric for socket-oriented
workloads, so clients and services can connect through a common control plane
instead of bespoke per-project tunnels.

## Core Node Types

All participants are nodes. A node has a keypair identity and a derived Node ID.

### RM (Resource Manager)

Role:
- Central router and session hub.

Responsibilities:
- Accept node connections.
- Negotiate session encoding during handshake.
- Route messages to destination nodes.
- Provide the backbone for client-to-service connectivity.

Mental model:
- RM is the switchboard. Most node-to-node interaction flows through it.

### RS (Resource Service)

Role:
- Service-side adapter that exposes resources to clients over RSP.

Responsibilities:
- Register as a service node.
- Handle socket operations such as listen/connect/send/recv/close on behalf of
  clients.
- Bridge between RSP sockets and real local processes or servers.

Examples in this repository:
- OpenSSH adapter (`rsp_sshd`).
- HTTP adapters used by Node.js and Python integrations.

### CS (Client Service)

Role:
- Client-side participant that initiates work and consumes remote resources.

Responsibilities:
- Connect to RM.
- Request endorsements when needed.
- Open and use RSP sockets to reach RS-provided resources.

Examples in this repository:
- C++ client code.
- Node.js client library and integration harnesses.
- Python client library.

### ES (Endorsement Service)

Role:
- Authorization metadata issuer.

Responsibilities:
- Issue endorsements that bind claims/roles/access to identities.
- Support authorization decisions across RM/RS/CS interactions.

Mental model:
- ES is an authority node that vouches for capabilities.

### NS (Name Service) - as-designed

Status:
- Not implemented yet.

Intended role:
- Map human-readable names to Node IDs.
- Support operational rerouting and failover via name indirection.

Important:
- Treat NS behavior as design intent until implementation lands.

## Major Subsystems

### Transport subsystem

What it is:
- The byte-stream connection layer between nodes and RM.

What it does:
- Opens/listens/connects at the OS socket level.
- Reads/writes framed bytes for the currently selected message encoding.
- Reports connection lifecycle events (connect, close, errors) upward.

Current implementation notes:
- TCP is the primary implemented transport for RM/node links.
- Transport abstractions live in `common/transport/*` and OS-specific socket
  implementations under `os/*`.

### Encoding subsystem

What it is:
- The serialization/framing layer that turns logical RSP messages into bytes and
  back.

What it does:
- Encodes outbound messages.
- Detects boundaries for inbound messages and decodes them.
- Allows edge clients/services to use a supported encoding while RM internally
  routes protobuf structures.

Current implementation notes:
- Encoding abstractions live in `common/encoding/*`.
- Protobuf and JSON paths are implemented in this repository.

### Message queue and processing subsystem

What it is:
- The staged message-processing pipeline used by nodes.

What it does:
- Handshake negotiation.
- Authentication and signature checks.
- Authorization checks (endorsements).
- Dispatch and routing to handlers.

Current implementation notes:
- Queue/auth/signing/authz logic is in `common/message_queue/*`.

### Endorsement subsystem

What it is:
- Authorization metadata model and issuance/validation flow.

What it does:
- Binds claims/roles/access to identities.
- Enables policy-style gating in client/service interactions.

Current implementation notes:
- Shared endorsement logic: `common/endorsement/*`.
- Service binary: `endorsement_service/*`.

## Protocol Primer: Handshake, Encoding, and Flow

### What is the ASCII transport/handshake phase?

The first phase of a new RM session is a simple ASCII text exchange used to
select encoding and complete session setup. This phase is line-oriented and ends
with a blank line (`\r\n\r\n`).

Why it exists:
- Makes bring-up/debug easy with minimal tooling.
- Lets constrained clients pick an encoding before binary framing starts.

After this phase completes, the session switches to the chosen encoding and all
subsequent framing rules come from that encoding.

### What is an encoding in practice?

An encoding defines:
- Message serialization format.
- Frame boundary detection rules on the byte stream.
- Conversion to/from internal protobuf representation where needed.

Once selected, both sides must speak that encoding for the remainder of the
session unless the connection is restarted.

### End-to-end protocol flow

1. Node opens transport connection to RM (typically TCP).
2. ASCII handshake negotiates/accepts encoding.
3. Node identity/auth data is established for session processing.
4. Node sends signed/authenticated RSP messages.
5. RM routes messages by destination Node ID.
6. RS/CS perform socket operations via control messages.
7. Socket payload transfer proceeds as async data events.

### Socket operation flow (typical)

Client-facing sequence for a connect/send path:
1. CS sends `connect_tcp` request toward target RS.
2. RS creates/uses local socket and returns socket status/reply.
3. CS sends `socket_send` messages for payload bytes.
4. RS emits async socket data/status replies.
5. Either side issues `socket_close` to terminate socket lifecycle.

## How the system works end-to-end

Typical flow:
1. RM starts and accepts node sessions.
2. ES and RS nodes connect to RM.
3. A CS connects to RM.
4. CS obtains endorsements from ES (if required by policy).
5. CS sends socket operations toward RS via RM routing.
6. RS executes or proxies the operation and returns results/data.
7. Application traffic flows through RS <-> RM <-> CS as RSP messages and
   socket data events.

## Control plane and data plane

Control-plane style traffic:
- Session setup.
- Identity/endorsement exchange.
- Socket lifecycle operations (connect/listen/close).

Data-plane style traffic:
- Socket payload transfer after connection establishment.

In practice, both are represented as RSP message flows plus async socket events.

## Identity, trust, and security model

Implemented behavior:
- Messages are authenticated and integrity-protected.
- Authorization is endorsement-driven.
- Transport confidentiality is not guaranteed by RSP itself.

Implication:
- Application protocols should provide confidentiality where needed (for example
  SSH/TLS over RSP transport).

## Identity primitives (implemented)

### What is a keypair?

In this implementation, a keypair is an asymmetric signing identity represented
by `rsp::KeyPair` (`common/keypair.*`).

- Default/generated key type: ECDSA P-256 (`prime256v1`).
- Public keys are carried in `PublicKey` protobuf messages.
- Sign/verify operations use OpenSSL EVP APIs.

The protocol enum includes other algorithm IDs (`RSA2048`, `RSA4096`), but the
current keypair implementation enforces P-256 keys for active signing identity
paths.

### What is a NodeID?

A NodeID is a compact 128-bit identifier derived from a node's public key.

Implemented derivation (`KeyPair::nodeID()`):
1. Serialize public key to DER (`i2d_PUBKEY`).
2. Compute SHA-256 digest of DER bytes.
3. Take the first 16 bytes (128 bits) of the digest.
4. Split into two 64-bit halves (`high`, `low`) to form `NodeID`.

This makes NodeID deterministic for a given public key.

## Identity flow and challenge flow (implemented)

Identity establishment is challenge-based, not trust-on-first-message.

Handshake behavior in `MessageQueueAuthN` (`common/message_queue/mq_authn.cpp`):
1. Side A sends `challenge_request` with a random 16-byte nonce.
2. Side B replies with an `identity` message containing:
   - the received nonce
   - B's public key
   - signature over the identity message
3. Side A verifies:
   - nonce equality (prevents replay/cross-session confusion)
   - signature validity against the included public key
   - derived NodeID from that public key
4. The peer identity is accepted and can be cached.

The handshake is symmetric: each side must both answer the peer challenge and
receive a valid answer to its own challenge.

### Identity cache behavior

`IdentityCache` (`common/node.cpp`) stores identities keyed by derived NodeID.

Key points:
- Cache entries keep the public key payload and drop nonce-specific challenge
  context (`sanitizedIdentityForCache`).
- Cache is LRU-bounded (`maximumEntries`, least-recent eviction).
- Cache can actively emit a targeted `challenge_request` when a node wants to
  refresh/confirm identity for a peer (`sendChallengeRequest`).

## Pre-sending identity and efficiency

There are two practical patterns:

1. Reactive identity send (currently implemented in client endorsement flow)
- In `client/cpp/rsp_client.cpp`, if ES replies
  `ENDORSEMENT_UNKNOWN_IDENTITY`, the client sends an `identity` message and
  retries the endorsement request.
- Benefit: avoids sending identity when not needed.
- Cost: adds one extra request/response cycle when peer identity cache is cold.

2. Pre-sent identity (optimization pattern)
- Client sends identity proactively before first operation to a peer likely not
  to have it cached (for example first contact after restart).
- Benefit: can eliminate the `UNKNOWN_IDENTITY` repair round-trip.
- Cost: extra message overhead when peer already has cached identity.

Rule of thumb:
- Low-latency or short-lived workflows benefit from pre-sending identity.
- Long-lived sessions with warm caches often benefit from reactive send.

This repository currently demonstrates the reactive repair path in the C++
client and challenge-based identity validation in the authN queue.

## Signature calculation algorithm (implemented)

RSP does not sign raw protobuf wire bytes. It signs a canonical message hash
computed by `computeMessageHash()` in `common/message_queue/mq_signing.cpp`.

Algorithm summary:
1. Build a canonical hash stream by visiting RSP message fields in defined order.
2. For each field, feed a numeric field tag and normalized value encoding.
3. Use SHA-256 over that canonical stream.
4. Sign the resulting 32-byte hash input with keypair signing.

Signing details:
- `signMessage()` -> `keyPair.signBlock(messageSignatureInput(message))`
- `messageSignatureInput()` is the 32-byte canonical SHA-256 message hash.
- `KeyPair::sign()` uses `EVP_DigestSign*` with `EVP_sha256()` and the P-256
  private key.
- Signature block includes:
  - signer NodeID
  - algorithm enum (`P256`)
  - signature bytes

Verification details:
- Recompute canonical hash from received message.
- Verify signature bytes against signer key/public key.
- Signature metadata is carried in `SignatureBlock` (`messages.proto`).

### Worked example (conceptual)

Suppose a CS emits a signed `connect_tcp_request` message:

1. Build logical message fields:
  - `destination = <RS node id>`
  - `source = <CS node id>`
  - `connect_tcp_request.host_port = "127.0.0.1:22"`
  - `nonce = <uuid>`

2. Canonical hashing step (`computeMessageHash`):
  - Feed field tags and normalized values in the implementation-defined order.
  - Include the selected submessage fields (`connect_tcp_request` in this case).
  - Exclude outer `RSPMessage.signature` while hashing.
  - Produce a 32-byte SHA-256 digest.

3. Signature step (`signMessage` / `KeyPair::signBlock`):
  - Sign the 32-byte digest with the sender's P-256 private key.
  - Emit `SignatureBlock`:
    - `signer = <sender node id bytes>`
    - `algorithm = P256`
    - `signature = <ecdsa signature bytes>`

4. Receiver verification:
  - Recompute canonical digest from received message content.
  - Resolve signer identity/public key.
  - Verify signature bytes against recomputed digest.

If the digest or signer key does not match, verification fails and the message
is rejected by signature-checking stages.

## Session and encoding model

- Sessions begin with an ASCII handshake.
- Peers agree on an encoding for subsequent messages.
- RM internal routing logic works on protobuf message structures.
- Generated Node.js and Python message bindings are maintained in this repo.

## As-built repository mapping

Major implemented binaries and stacks:
- `resource_manager`
- `resource_service`
- `endorsement_service`
- C++ client libraries
- Node.js client libraries and integration tests
- Python client libraries and integration tests

Implemented integration examples:
- OpenSSH over RSP (`rsp_ssh` and `rsp_sshd`)
- Node.js Express over RSP
- Python HTTP server over RSP

## What this primer is and is not

This primer is:
- A system-level map of roles, boundaries, and intent.
- Grounded in current implementation behavior.

This primer is not:
- A full protocol reference.
- A replacement for module-level docs and test harness docs.

For day-to-day work, pair this file with README and the integration-specific
documentation under `integration/*/modification/`.
