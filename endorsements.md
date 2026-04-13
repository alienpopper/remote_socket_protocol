# Endorsements Primer (As-Built)

This document explains the endorsement model used by this repository so a user,
developer, or LLM can reason about authorization behavior end to end.

## Philosophy

Endorsements are signed, portable authorization facts.

The core philosophy is:
- Authorization should be message-oriented, not tied to one long-lived socket.
- Claims should be delegated by an authority node (ES) and verifiable offline.
- Nodes should be able to cache claims and reuse them safely until expiration.

In practice, endorsements let RSP nodes decide "is this sender allowed to do
this" without requiring every message to round-trip to an authorization server.

## Why endorsements are cacheable

Endorsements are designed to be cacheable because they contain enough
cryptographic context to be verified locally.

Each endorsement includes:
- Subject Node ID (who the claim applies to)
- Endorsement service Node ID (who issued it)
- Endorsement type
- Endorsement value
- Expiration time (`valid_until`)
- Signature by the endorsement service

Because signature and expiry are embedded, a node can cache an endorsement and
reuse it for future authorization checks until it expires.

## Endorsement structure

Protocol structure (`messages.proto`, `Endorsement`):
- `subject: NodeId`
- `endorsement_service: NodeId`
- `endorsement_type: Uuid`
- `endorsement_value: bytes`
- `valid_until: DateTime`
- `signature: bytes`

Implementation references:
- API: `common/endorsement/endorsement.hpp`
- Implementation: `common/endorsement/endorsement.cpp`

Key operations implemented:
- `createSigned(...)`
- `verifySignature(...)`
- `serialize()` / `deserialize()`
- `toProto()` / `fromProto()`

## Well-known endorsement constants

Source of truth in C++: `common/endorsement/well_known_endorsements.h`.

Access constants:
- `ETYPE_ACCESS = f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b`
- `EVALUE_ACCESS_NETWORK = f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b`
- `EVALUE_REGISTER_NAMES = eaf1a839-124e-4d3e-8ece-35663ede1054`

Role constants:
- `ETYPE_ROLE = 0963c0ab-215f-42c1-b042-747bf21e330e`
- `EVALUE_ROLE_CLIENT = edab2025-4ae1-44f2-a683-1a390586e10c`
- `EVALUE_ROLE_RESOURCE_MANAGER = d1c9b8e7-5a0c-4f1e-9c3a-2b6f8e7c9a2f`
- `EVALUE_ROLE_RESOURCE_SERVICE = a7f8c9d6-3b2e-4f1a-8c9d-5e6f7a8b9c0d`
- `EVALUE_ROLE_ENDORSEMENT_SERVICE = c3d4e5f6-7a8b-9c0d-1e2f-3a4b5c6d7e8f`
- `EVALUE_ROLE_NAME_SERVICE = 083580cc-ecae-4756-896f-236438800d55`

Note:
- Node.js/Python integration tests currently mirror these GUID values directly
  in test/app code paths.

## Endorsement requirement AST

When authorization fails or when policy is evaluated, requirements are expressed
as `ERDAbstractSyntaxTree` (`messages.proto`).

Supported node families:
- Boolean/composition: `equals`, `and`, `or`, `all_of`, `any_of`, `true`, `false`
- Endorsement predicates: `endorsement_type_equals`,
  `endorsement_value_equals`, `endorsement_signer_equals`
- Message predicates: `message_source`, `message_destination`

Why this matters:
- The AST is the machine-readable description of what authorization evidence is
  still required.

## How AST reduction works

Reduction API:
- `reduceRequirementTree(tree, endorsements, message?)`

Behavior summary (as implemented):
1. Constant `true` reduces to empty tree (satisfied).
2. Constant `false` stays `false` (unsatisfied).
3. If an endorsement matches a branch, that branch is removed.
4. If message predicates are present and a message context is provided,
   `message_source`/`message_destination` branches are reduced using that
   message.
5. Composite nodes are simplified recursively:
   - `and`: any false branch -> false; satisfied branches are removed.
   - `or`: any satisfied branch -> satisfied; false branches are removed.
   - `all_of`/`any_of`: reduced term-by-term with equivalent logic.

Interpretation convention used in this repo:
- Empty AST (`NODE_TYPE_NOT_SET`) means authorization requirements are fully
  satisfied.
- Non-empty AST means some requirement remains unmet.
- `false` AST means unsatisfiable under current endorsements/message context.

## Client-visible flow

Typical endorsement flow:
1. Client sends a signed request message.
2. Authorization stage evaluates request using available endorsements.
3. If requirements are unmet, server may respond with `endorsement_needed` and
   a requirement AST.
4. Client acquires missing endorsement(s) from ES using
   `begin_endorsement_request` (with optional challenge/reply stages).
5. Client retries original request with updated cached endorsements.
6. Requirement tree reduces to empty and request is accepted.

## Practical caveat (important)

There is a known development caveat in `messages.proto`:
- Requirement matching can satisfy type/value/signer checks across different
  endorsements instead of enforcing all predicates on one endorsement object.

This behavior is explicitly marked in-proto as a TODO and should be kept in mind
for threat modeling and future hardening.

## Build and test entry points

Build endorsement service:

```bash
make build/bin/endorsement_service
```

Run tests that exercise endorsement behavior:

```bash
make test
make test-nodejs-client
make test-nodejs-express
make test-python-http-server
```
