# ResourceQuery Language

## Why Replace ResourceRecord-Based Discovery

`ResourceRecord` discovery is tied to a fixed set of compile-time resource variants such as `tcp_connect`, `tcp_listen`, and `sshd`. That model conflicts with the newer direction of the project, where services can advertise their runtime schema and accepted message types dynamically.

The result is an awkward split:

- `ServiceSchema` already describes what a service can accept at runtime.
- `ResourceQuery` still returns legacy `ResourceRecord` values.
- Schema-only services, such as the name service, are poorly represented by `ResourceRecord` and should not need fake records just to participate in discovery.

The replacement direction is to make discovery operate on service instances and their advertised schemas, not on hard-coded transport-specific resource records.

## Discovery Model

The unit of discovery is a single advertised service schema on a specific node.

Each discovered service entry contains:

- `node.id`
- `service.proto_file_name`
- `service.schema_hash`
- `service.schema_version`
- `service.accepted_type_urls`

This is intentionally narrow for v1. It is enough to discover different classes of services without introducing a second static type system on top of the runtime schema system.

## Query Language Goals

- Work against runtime-advertised schemas rather than `ResourceRecord` variants.
- Be easy to parse and evaluate inside the resource manager.
- Be expressive enough for service-type discovery without becoming a general-purpose language.
- Leave room for future metadata-based queries.

## Supported Query Fields

Version 1 supports these field paths:

- `node.id`
- `service.proto_file_name`
- `service.schema_hash`
- `service.schema_version`
- `service.accepted_type_urls`

## Supported Operators

- `=`
- `!=`
- `HAS`
- `AND`
- `OR`
- `NOT`
- parentheses: `(` and `)`

Operator semantics:

- `=` and `!=` compare scalar fields.
- `HAS` checks membership in repeated string fields. In v1, this is intended for `service.accepted_type_urls`.
- `AND`, `OR`, and `NOT` provide boolean composition.

## Query Syntax

The language is a small boolean expression language.

Examples:

```text
service.proto_file_name = "name_service.proto"
```

```text
service.accepted_type_urls HAS "type.rsp/rsp.proto.NameReadRequest"
```

```text
service.proto_file_name = "sshd.proto" AND service.accepted_type_urls HAS "type.rsp/rsp.proto.ConnectSshd"
```

```text
NOT service.proto_file_name = "sshd.proto"
```

```text
(service.proto_file_name = "name_service.proto" OR service.proto_file_name = "sshd.proto")
AND node.id != "00000000-0000-0000-0000-000000000000"
```

### Informal Grammar

```text
expr      := or_expr
or_expr   := and_expr (OR and_expr)*
and_expr  := unary_expr (AND unary_expr)*
unary_expr:= NOT unary_expr | primary
primary   := comparison | '(' expr ')'
comparison:= field operator literal
field     := identifier ('.' identifier)*
operator  := '=' | '!=' | HAS
literal   := quoted string | integer
```

## Result Shape

The RM should not answer a schema-first query with a `ResourceAdvertisement` full of legacy records.

Instead, the reply contains a list of discovered services:

- `node_id`
- `schema`

Each result represents one node/schema pair that matched the query.

This keeps the reply directly aligned with the query model and avoids coupling query results to `ResourceRecord`.

## Matching Rules

- A service entry matches when its single advertised schema satisfies the boolean expression.
- `service.accepted_type_urls HAS "..."` is true when any accepted type URL equals the string.
- `node.id` is matched against the canonical UUID string form.
- `service.schema_hash` is matched as a hex string.
- An empty query means match all advertised service schemas.

## Migration Plan

The implementation should move in stages:

1. Keep `ResourceAdvertisement` for service announcement.
2. Stop using `ResourceRecord` as the query result model.
3. Add a dedicated `ResourceQueryReply` containing discovered service entries.
4. Evaluate queries against node/schema pairs.
5. Preserve `ResourceRecord` only for existing transport/resource-service behavior until those callers are replaced.
6. Eventually deprecate and remove `ResourceRecord` from discovery.

## Future Extensions

If the project later needs richer filtering, extend service advertisements with explicit discovery metadata rather than adding more `ResourceRecord` variants.

Possible future additions:

- `service.kind`
- typed metadata fields
- version comparisons such as `>=`
- `EXISTS(field)`
- prefix or substring matching

Those should be added only after the schema-first path has proven stable.