#!/usr/bin/env python3
"""Generate messages.js and messages.py from messages.proto.

Usage:
    python3 scripts/generate_messages.py \
        --proto messages.proto \
        --nodejs client/nodejs/messages.js \
        --python client/python/messages.py
"""

import argparse
import json
import re
import sys

SCALAR_TYPES = frozenset({
    "bool", "bytes", "double", "fixed32", "fixed64", "float",
    "int32", "int64", "sfixed32", "sfixed64", "sint32", "sint64",
    "string", "uint32", "uint64",
})


def strip_comments(text):
    """Remove // line comments and /* */ block comments."""
    result = []
    i = 0
    while i < len(text):
        if text[i:i+2] == "//":
            # Skip to end of line
            while i < len(text) and text[i] != "\n":
                i += 1
        elif text[i:i+2] == "/*":
            # Skip block comment
            end = text.find("*/", i + 2)
            if end == -1:
                break
            i = end + 2
        else:
            result.append(text[i])
            i += 1
    return "".join(result)


def extract_top_level_blocks(text, keyword):
    """Find all top-level 'keyword Name { ... }' blocks. Returns list of (name, body)."""
    blocks = []
    pattern = re.compile(r"\b" + keyword + r"\s+(\w+)\s*\{")
    pos = 0
    while True:
        m = pattern.search(text, pos)
        if not m:
            break
        name = m.group(1)
        start = m.end()  # position after opening '{'
        depth = 1
        i = start
        while i < len(text) and depth > 0:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        body = text[start:i - 1]
        blocks.append((name, body))
        pos = i
    return blocks


def parse_enums(text):
    """Parse all top-level enum blocks. Returns {name: {VALUE: number}}."""
    enums = {}
    for name, body in extract_top_level_blocks(text, "enum"):
        values = {}
        for m in re.finditer(r"(\w+)\s*=\s*(-?\d+)\s*;", body):
            values[m.group(1)] = int(m.group(2))
        enums[name] = values
    return enums


def parse_messages(text, enums):
    """Parse all top-level message blocks. Returns ordered dict of message schemas."""
    messages = {}
    for msg_name, msg_body in extract_top_level_blocks(text, "message"):
        fields = []
        oneofs = []

        # Find oneof sub-blocks within message body
        oneof_pattern = re.compile(r"\boneof\s+(\w+)\s*\{")
        oneof_bodies = {}  # oneof_name -> [field_names]
        # Track positions consumed by oneofs so we can exclude them from regular parsing
        oneof_spans = []
        pos = 0
        while True:
            m = oneof_pattern.search(msg_body, pos)
            if not m:
                break
            oneof_name = m.group(1)
            start = m.end()
            depth = 1
            i = start
            while i < len(msg_body) and depth > 0:
                if msg_body[i] == "{":
                    depth += 1
                elif msg_body[i] == "}":
                    depth -= 1
                i += 1
            oneof_body = msg_body[start:i - 1]
            oneof_spans.append((m.start(), i))
            oneof_bodies[oneof_name] = oneof_body
            pos = i

        # Parse fields inside each oneof
        for oneof_name, oneof_body in oneof_bodies.items():
            field_names_in_oneof = []
            for m in re.finditer(
                r"(repeated\s+|optional\s+)?(\w+)\s+(\w+)\s*=\s*(\d+)\s*(?:\[[^\]]*\])?\s*;",
                oneof_body,
            ):
                _qualifier, type_name, field_name, number = (
                    m.group(1), m.group(2), m.group(3), int(m.group(4))
                )
                repeated = bool(_qualifier and _qualifier.strip() == "repeated")
                kind = _field_kind(type_name, enums, messages)
                field = {
                    "name": field_name,
                    "number": number,
                    "kind": kind,
                    "type": type_name,
                    "repeated": repeated,
                    "has_presence": True,  # oneof fields always have presence
                    "oneof": oneof_name,
                }
                fields.append(field)
                field_names_in_oneof.append(field_name)
            oneofs.append({"name": oneof_name, "fields": field_names_in_oneof})

        # Build the body with oneof spans removed so we only parse regular fields
        body_no_oneofs = _remove_spans(msg_body, oneof_spans)

        # Parse regular fields (not inside oneofs)
        for m in re.finditer(
            r"(repeated\s+|optional\s+)?(\w+)\s+(\w+)\s*=\s*(\d+)\s*(?:\[[^\]]*\])?\s*;",
            body_no_oneofs,
        ):
            qualifier = m.group(1)
            type_name = m.group(2)
            field_name = m.group(3)
            number = int(m.group(4))

            # Skip proto options that match the pattern (e.g. "optimize_for = LITE_RUNTIME")
            if type_name in ("syntax", "package", "option", "optimize_for"):
                continue

            repeated = bool(qualifier and qualifier.strip() == "repeated")
            optional = bool(qualifier and qualifier.strip() == "optional")
            kind = _field_kind(type_name, enums, messages)
            has_presence = optional or kind == "message"

            field = {
                "name": field_name,
                "number": number,
                "kind": kind,
                "type": type_name,
                "repeated": repeated,
                "has_presence": has_presence,
                "oneof": None,
            }
            fields.append(field)

        # Sort by field number to match proto ordering
        fields.sort(key=lambda f: f["number"])

        messages[msg_name] = {"fields": fields, "oneofs": oneofs}

    return messages


def _field_kind(type_name, enums, messages):
    if type_name in SCALAR_TYPES:
        return "scalar"
    if type_name in enums:
        return "enum"
    return "message"


def _remove_spans(text, spans):
    """Return text with the given (start, end) spans replaced by spaces."""
    if not spans:
        return text
    result = list(text)
    for start, end in spans:
        for i in range(start, min(end, len(result))):
            result[i] = " "
    return "".join(result)


def parse_proto(proto_path):
    with open(proto_path, "r", encoding="utf-8") as f:
        text = f.read()
    # Resolve import directives relative to the proto file's parent directory.
    import os
    base_dir = os.path.dirname(proto_path) or "."
    for m in re.finditer(r'import\s+"([^"]+)"\s*;', text):
        import_path = os.path.join(base_dir, m.group(1))
        if os.path.isfile(import_path):
            with open(import_path, "r", encoding="utf-8") as f2:
                text += "\n" + f2.read()
    text = strip_comments(text)
    enums = parse_enums(text)
    messages = parse_messages(text, enums)
    return enums, messages


def parse_protos(proto_paths):
    """Parse multiple proto files and merge their schemas."""
    combined_text = ""
    import os
    seen = set()
    for proto_path in proto_paths:
        abs_path = os.path.abspath(proto_path)
        if abs_path in seen:
            continue
        seen.add(abs_path)
        with open(proto_path, "r", encoding="utf-8") as f:
            combined_text += "\n" + f.read()
        base_dir = os.path.dirname(proto_path) or "."
        for m in re.finditer(r'import\s+"([^"]+)"\s*;', combined_text):
            import_path = os.path.join(base_dir, m.group(1))
            abs_import = os.path.abspath(import_path)
            if abs_import not in seen and os.path.isfile(import_path):
                seen.add(abs_import)
                with open(import_path, "r", encoding="utf-8") as f2:
                    combined_text += "\n" + f2.read()
    combined_text = strip_comments(combined_text)
    enums = parse_enums(combined_text)
    messages = parse_messages(combined_text, enums)
    return enums, messages


# ---------------------------------------------------------------------------
# JS output
# ---------------------------------------------------------------------------

JS_BOILERPLATE = r"""const MESSAGE_TYPES = SCHEMA.messages;
const ENUM_TYPES = SCHEMA.enums;
const FIELD_NAME_LOOKUP = new Map(
    Object.entries(MESSAGE_TYPES).map(([typeName, definition]) => [
        typeName,
        new Set(definition.fields.map((field) => field.name)),
    ])
);

function hasOwn(object, key) {
    return Object.prototype.hasOwnProperty.call(object, key);
}

function fieldPresent(object, key) {
    return object !== null && typeof object === "object" && hasOwn(object, key) && object[key] !== undefined && object[key] !== null;
}

function fail(path, message) {
    throw new Error(`${path}: ${message}`);
}

function isPlainObject(value) {
    return value !== null && typeof value === "object" && !Array.isArray(value);
}

function normalizeBytes(value, path) {
    if (Buffer.isBuffer(value) || value instanceof Uint8Array) {
        return Buffer.from(value).toString("base64");
    }
    if (typeof value !== "string") {
        fail(path, "expected bytes as a base64 string, Buffer, or Uint8Array");
    }
    return value;
}

function normalizeBoolean(value, path) {
    if (typeof value !== "boolean") {
        fail(path, "expected a boolean");
    }
    return value;
}

function normalizeString(value, path) {
    if (typeof value !== "string") {
        fail(path, "expected a string");
    }
    return value;
}

function normalizeInteger(value, path, minimum, maximum) {
    let numericValue = value;
    if (typeof numericValue === "string") {
        numericValue = Number(numericValue);
    }
    if (typeof numericValue === "bigint") {
        if (numericValue < BigInt(minimum) || numericValue > BigInt(maximum)) {
            fail(path, `expected an integer in range [${minimum}, ${maximum}]`);
        }
        if (numericValue > BigInt(Number.MAX_SAFE_INTEGER) || numericValue < BigInt(Number.MIN_SAFE_INTEGER)) {
            fail(path, "JSON encoding cannot safely represent integers outside the JavaScript safe integer range");
        }
        numericValue = Number(numericValue);
    }
    if (typeof numericValue !== "number" || !Number.isInteger(numericValue) || numericValue < minimum || numericValue > maximum) {
        fail(path, `expected an integer in range [${minimum}, ${maximum}]`);
    }
    return numericValue;
}

function normalizeEnum(enumName, value, path) {
    const enumDefinition = ENUM_TYPES[enumName];
    if (!enumDefinition) {
        fail(path, `unknown enum ${enumName}`);
    }
    let numericValue = value;
    if (typeof numericValue === "string") {
        if (!hasOwn(enumDefinition, numericValue)) {
            fail(path, `unknown enum value ${numericValue} for ${enumName}`);
        }
        numericValue = enumDefinition[numericValue];
    }
    numericValue = normalizeInteger(numericValue, path, -0x80000000, 0xffffffff);
    if (!Object.values(enumDefinition).includes(numericValue)) {
        fail(path, `unknown enum value ${numericValue} for ${enumName}`);
    }
    return numericValue;
}

function defaultScalarValue(field) {
    switch (field.kind) {
    case "enum":
        return 0;
    case "scalar":
        switch (field.type) {
        case "bool":
            return false;
        case "string":
        case "bytes":
            return "";
        default:
            return 0;
        }
    default:
        return undefined;
    }
}

function normalizeScalar(field, value, path) {
    switch (field.type) {
    case "bool":
        return normalizeBoolean(value, path);
    case "string":
        return normalizeString(value, path);
    case "bytes":
        return normalizeBytes(value, path);
    case "uint32":
    case "fixed32":
        return normalizeInteger(value, path, 0, 0xffffffff);
    case "int32":
    case "sint32":
    case "sfixed32":
        return normalizeInteger(value, path, -0x80000000, 0x7fffffff);
    case "uint64":
    case "fixed64":
        return normalizeInteger(value, path, 0, Number.MAX_SAFE_INTEGER);
    case "int64":
    case "sint64":
    case "sfixed64":
        return normalizeInteger(value, path, Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER);
    default:
        fail(path, `unsupported scalar type ${field.type}`);
    }
}

function normalizeFieldValue(field, value, path) {
    switch (field.kind) {
    case "scalar":
        return normalizeScalar(field, value, path);
    case "enum":
        return normalizeEnum(field.type, value, path);
    case "message":
        return normalizeMessage(field.type, value, path);
    default:
        fail(path, `unsupported field kind ${field.kind}`);
    }
}

function normalizeMessage(typeName, value, path = typeName) {
    const definition = MESSAGE_TYPES[typeName];
    if (!definition) {
        fail(path, `unknown message type ${typeName}`);
    }

    const input = value === undefined || value === null ? {} : value;
    if (!isPlainObject(input)) {
        fail(path, "expected an object");
    }

    const knownFields = FIELD_NAME_LOOKUP.get(typeName);
    for (const inputField of Object.keys(input)) {
        if (!knownFields.has(inputField)) {
            fail(`${path}.${inputField}`, "unknown field");
        }
    }

    for (const oneof of definition.oneofs) {
        let presentCount = 0;
        for (const fieldName of oneof.fields) {
            if (fieldPresent(input, fieldName)) {
                presentCount += 1;
            }
        }
        if (presentCount > 1) {
            fail(path, `oneof ${oneof.name} has multiple values set`);
        }
    }

    const result = {};
    for (const field of definition.fields) {
        const present = fieldPresent(input, field.name);
        const fieldPath = `${path}.${field.name}`;

        if (field.repeated) {
            const listValue = present ? input[field.name] : [];
            if (!Array.isArray(listValue)) {
                fail(fieldPath, "expected an array");
            }
            result[field.name] = listValue.map((item, index) => normalizeFieldValue(field, item, `${fieldPath}[${index}]`));
            continue;
        }

        if (present) {
            result[field.name] = normalizeFieldValue(field, input[field.name], fieldPath);
            continue;
        }

        if (!field.has_presence) {
            result[field.name] = defaultScalarValue(field);
        }
    }

    return result;
}

function validateMessage(typeName, value) {
    try {
        normalizeMessage(typeName, value);
        return true;
    } catch {
        return false;
    }
}

function assertValidMessage(typeName, value) {
    normalizeMessage(typeName, value);
}

class MessageHasher {
    constructor() {
        this.hash = crypto.createHash("sha256");
    }

    feed(buffer) {
        this.hash.update(buffer);
    }

    feedUint8(value) {
        const buffer = Buffer.alloc(1);
        buffer.writeUInt8(value, 0);
        this.feed(buffer);
    }

    feedUint32(value) {
        const buffer = Buffer.alloc(4);
        buffer.writeUInt32BE(value >>> 0, 0);
        this.feed(buffer);
    }

    feedInt32(value) {
        const buffer = Buffer.alloc(4);
        buffer.writeInt32BE(value | 0, 0);
        this.feed(buffer);
    }

    feedUint64(value) {
        const buffer = Buffer.alloc(8);
        buffer.writeBigUInt64BE(BigInt.asUintN(64, BigInt(value)), 0);
        this.feed(buffer);
    }

    feedBool(value) {
        this.feedUint8(value ? 1 : 0);
    }

    feedBytes(value) {
        const buffer = Buffer.isBuffer(value) ? value : Buffer.from(value);
        this.feedUint32(buffer.length);
        this.feed(buffer);
    }

    tag(fieldNumber) {
        this.feedUint32(fieldNumber);
    }

    finalize() {
        return this.hash.digest();
    }
}

function hashScalar(field, value, hasher) {
    switch (field.type) {
    case "bool":
        hasher.feedBool(value);
        return;
    case "string":
        hasher.feedBytes(Buffer.from(value, "utf8"));
        return;
    case "bytes":
        hasher.feedBytes(Buffer.from(value, "base64"));
        return;
    case "uint32":
    case "fixed32":
        hasher.feedUint32(value);
        return;
    case "int32":
    case "sint32":
    case "sfixed32":
        hasher.feedInt32(value);
        return;
    case "uint64":
    case "fixed64":
        hasher.feedUint64(BigInt(value));
        return;
    case "int64":
    case "sint64":
    case "sfixed64":
        hasher.feedUint64(BigInt.asUintN(64, BigInt(value)));
        return;
    default:
        fail(field.type, "unsupported scalar type for hashing");
    }
}

function hashAny(value, hasher) {
    // Hash google.protobuf.Any by its type_url string, then hash the inner message fields.
    const typeUrl = value["@type"] || "";
    hasher.feedBytes(Buffer.from(typeUrl, "utf8"));
    const slash = typeUrl.lastIndexOf("/");
    const fullName = slash >= 0 ? typeUrl.substring(slash + 1) : typeUrl;
    // Strip package prefix to get the simple type name for schema lookup
    const dot = fullName.lastIndexOf(".");
    const typeName = dot >= 0 ? fullName.substring(dot + 1) : fullName;
    const definition = MESSAGE_TYPES[typeName];
    if (!definition) {
        fail(typeUrl, "cannot hash unknown Any type");
    }
    // Build a copy without @type for hashing as a regular message
    const inner = {};
    for (const key of Object.keys(value)) {
        if (key !== "@type") inner[key] = value[key];
    }
    hashMessageObject(typeName, inner, hasher);
}

function hashFieldValue(field, value, hasher) {
    switch (field.kind) {
    case "scalar":
        hashScalar(field, value, hasher);
        return;
    case "enum":
        hasher.feedUint32(value >>> 0);
        return;
    case "message":
        if (field.type === "Any") {
            hashAny(value, hasher);
            return;
        }
        hashMessageObject(field.type, value, hasher);
        return;
    default:
        fail(field.type, "unsupported field kind for hashing");
    }
}

function hashMessageObject(typeName, value, hasher) {
    const definition = MESSAGE_TYPES[typeName];
    for (const field of definition.fields) {
        if (field.name === "signature" && typeName === "RSPMessage") {
            // Signature fields are excluded from the hash — the hash is what gets signed.
            // Signing raw protobuf bytes instead would require a protobuf dependency in clients.
            continue;
        }

        if (field.repeated) {
            const listValue = value[field.name] || [];
            hasher.tag(field.number);
            hasher.feedUint32(listValue.length >>> 0);
            for (const item of listValue) {
                hashFieldValue(field, item, hasher);
            }
            continue;
        }

        if (field.has_presence && !fieldPresent(value, field.name)) {
            continue;
        }

        hasher.tag(field.number);
        hashFieldValue(field, value[field.name], hasher);
    }
}

function hashMessage(typeName, value) {
    const normalized = normalizeMessage(typeName, value);
    const hasher = new MessageHasher();
    hashMessageObject(typeName, normalized, hasher);
    return hasher.finalize();
}

const exported = {
    schema: Object.freeze(SCHEMA),
    constructors: {},
    validators: {},
    hashers: {},
    createMessage: normalizeMessage,
    validateMessage,
    assertValidMessage,
    hashMessage,
};

for (const [enumName, values] of Object.entries(ENUM_TYPES)) {
    exported[enumName] = Object.freeze({...values});
}

for (const typeName of Object.keys(MESSAGE_TYPES)) {
    const createName = `create${typeName}`;
    const validateName = `validate${typeName}`;
    const assertName = `assertValid${typeName}`;
    const hashName = `hash${typeName}`;

    exported[createName] = (value = {}) => normalizeMessage(typeName, value);
    exported[validateName] = (value) => validateMessage(typeName, value);
    exported[assertName] = (value) => assertValidMessage(typeName, value);
    exported[hashName] = (value) => hashMessage(typeName, value);

    exported.constructors[typeName] = exported[createName];
    exported.validators[typeName] = exported[validateName];
    exported.hashers[typeName] = exported[hashName];
}

module.exports = Object.freeze(exported);
"""


def generate_js(enums, messages, output_path):
    schema = {"enums": enums, "messages": messages}
    schema_json = json.dumps(schema, indent=4)
    # Indent each line of schema JSON by 0 (already formatted by json.dumps with indent)
    output = (
        '"use strict";\n\n'
        "// Generated from messages.proto by scripts/generate_messages.py.\n"
        "// Do not edit this file directly.\n\n"
        'const crypto = require("crypto");\n\n'
        f"const SCHEMA = {schema_json};\n"
        f"{JS_BOILERPLATE}"
    )
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(output)


# ---------------------------------------------------------------------------
# Python output
# ---------------------------------------------------------------------------

PY_BOILERPLATE = '''
_SCALAR_TYPES = frozenset({
    "bool", "bytes", "double", "fixed32", "fixed64", "float",
    "int32", "int64", "sfixed32", "sfixed64", "sint32", "sint64",
    "string", "uint32", "uint64",
})


class _MessageHasher:
    def __init__(self) -> None:
        self._h = hashlib.sha256()

    def feed(self, data: bytes) -> None:
        self._h.update(data)

    def feed_uint8(self, value: int) -> None:
        self.feed(struct.pack(">B", value & 0xFF))

    def feed_uint32(self, value: int) -> None:
        self.feed(struct.pack(">I", value & 0xFFFFFFFF))

    def feed_int32(self, value: int) -> None:
        self.feed(struct.pack(">i", value & 0xFFFFFFFF))

    def feed_uint64(self, value: int) -> None:
        self.feed(struct.pack(">Q", value & 0xFFFFFFFFFFFFFFFF))

    def feed_bool(self, value: bool) -> None:
        self.feed_uint8(1 if value else 0)

    def feed_bytes(self, value: bytes) -> None:
        self.feed_uint32(len(value))
        self.feed(value)

    def tag(self, field_number: int) -> None:
        self.feed_uint32(field_number)

    def finalize(self) -> bytes:
        return self._h.digest()


def _field_present(value: dict, name: str) -> bool:
    return value is not None and name in value and value[name] is not None


def _hash_scalar(field: dict, value: Any, hasher: _MessageHasher) -> None:
    t = field["type"]
    if value is None:
        if t == "bool":
            value = False
        elif t == "string":
            value = ""
        elif t == "bytes":
            value = b""
        else:
            value = 0
    if t == "bool":
        hasher.feed_bool(bool(value))
    elif t == "string":
        hasher.feed_bytes(value.encode("utf-8"))
    elif t == "bytes":
        hasher.feed_bytes(base64.b64decode(value) if isinstance(value, str) else bytes(value))
    elif t in ("uint32", "fixed32"):
        hasher.feed_uint32(int(value))
    elif t in ("int32", "sint32", "sfixed32"):
        hasher.feed_int32(int(value))
    elif t in ("uint64", "fixed64", "int64", "sint64", "sfixed64"):
        hasher.feed_uint64(int(value) & 0xFFFFFFFFFFFFFFFF)
    else:
        raise ValueError(f"unsupported scalar type: {t}")


def _hash_field_value(field: dict, value: Any, hasher: _MessageHasher) -> None:
    k = field["kind"]
    if k == "scalar":
        _hash_scalar(field, value, hasher)
    elif k == "enum":
        hasher.feed_uint32(int(value) & 0xFFFFFFFF)
    elif k == "message":
        _hash_message_object(field["type"], value, hasher)
    else:
        raise ValueError(f"unsupported field kind: {k}")


def _hash_message_object(type_name: str, value: dict, hasher: _MessageHasher) -> None:
    definition = _SCHEMA["messages"][type_name]
    for field in definition["fields"]:
        if field["name"] == "signature" and type_name == "RSPMessage":
            continue
        if field["repeated"]:
            list_value = (value or {}).get(field["name"]) or []
            hasher.tag(field["number"])
            hasher.feed_uint32(len(list_value))
            for item in list_value:
                _hash_field_value(field, item, hasher)
            continue
        if field["has_presence"] and not _field_present(value, field["name"]):
            continue
        hasher.tag(field["number"])
        _hash_field_value(field, (value or {}).get(field["name"]), hasher)


def hash_message(type_name: str, value: dict) -> bytes:
    """Compute the canonical SHA-256 hash of a message object."""
    hasher = _MessageHasher()
    _hash_message_object(type_name, value, hasher)
    return hasher.finalize()


def hash_rsp_message(value: dict) -> bytes:
    """Canonical hash of an RSPMessage (excludes signature field)."""
    return hash_message("RSPMessage", value)


def hash_endorsement(value: dict) -> bytes:
    """Canonical hash of an Endorsement (includes signature field)."""
    return hash_message("Endorsement", value)
'''


def generate_py(enums, messages, output_path):
    lines = [
        "# Generated from messages.proto by scripts/generate_messages.py.",
        "# Do not edit this file directly.",
        "",
        '"""RSP message schema and canonical hash functions for Python clients."""',
        "",
        "import base64",
        "import hashlib",
        "import struct",
        "import sys",
        "from enum import IntEnum",
        "from typing import Any",
        "",
        "",
        "# --- Enums ---",
        "",
    ]

    for enum_name, values in enums.items():
        lines.append(f"class {enum_name}(IntEnum):")
        for val_name, val_num in values.items():
            lines.append(f"    {val_name} = {val_num}")
        lines.append("")
        lines.append("")

    lines.append("# --- Schema ---")
    lines.append("")
    schema = {"enums": enums, "messages": messages}
    import pprint
    schema_repr = pprint.pformat(schema, indent=4, width=120)
    lines.append(f"_SCHEMA: dict = {schema_repr}")
    lines.append("")

    lines.append("_ENUM_TYPES: dict[str, type] = {")
    for enum_name in enums:
        lines.append(f'    "{enum_name}": {enum_name},')
    lines.append("}")
    lines.append("")
    lines.append("")
    lines.append("# --- Canonical hash ---")
    lines.append("")

    lines.append(PY_BOILERPLATE.lstrip("\n"))

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proto", required=True, nargs="+", help="Path(s) to .proto files")
    parser.add_argument("--nodejs", required=True, help="Output path for messages.js")
    parser.add_argument("--python", required=True, help="Output path for messages.py")
    args = parser.parse_args()

    enums, messages = parse_protos(args.proto)

    generate_js(enums, messages, args.nodejs)
    print(f"Generated {args.nodejs}", file=sys.stderr)

    generate_py(enums, messages, args.python)
    print(f"Generated {args.python}", file=sys.stderr)


if __name__ == "__main__":
    main()
