'use strict';

// Protobuf binary encoding plugin for the RSP full client.
//
// Implements the Encoding interface:
//   get handshakeToken()                – 'encoding:protobuf' (what the server calls kAsciiHandshakeEncoding)
//   readFrame(reader)  -> Promise<obj>  – decode binary RSPMessage to plain JS (JSON-compatible)
//   writeFrame(send, message)           – encode plain JS to binary RSPMessage frame
//
// Wire format: [magic:4BE][length:4BE][binary-RSPMessage:length]
// Magic: 0x52535050  ('RSPP')
//
// The internal message representation is kept as plain JS objects identical to what
// JSON encoding produces.  This means signing, hashing, and dispatch code are all
// encoding-agnostic.  The Any service_message is normalized:
//   recv: { type_url, value } binary Any  →  { '@type': typeUrl, ...unpackedFields }
//   send: { '@type': typeUrl, ...fields } →  { type_url, value: binaryInnerMsg }

const FRAME_MAGIC = 0x52535050; // 'RSPP'
const MAX_FRAME_LENGTH = 16 * 1024 * 1024;

// The fully-qualified name of the top-level RSP message type in messages.proto.
const RSP_MESSAGE_TYPE = 'rsp.proto.RSPMessage';

class ProtobufEncoding {
    // registry – a ProtoRegistry instance that has already loaded messages.proto
    constructor(registry) {
        this._registry = registry;
    }

    get handshakeToken() {
        return 'encoding:protobuf';
    }

    // --- Frame I/O ---

    async readFrame(reader) {
        const header = await reader.readExact(8);
        const magic = header.readUInt32BE(0);
        if (magic !== FRAME_MAGIC) {
            throw new Error(`protobuf encoding: bad frame magic 0x${magic.toString(16).padStart(8, '0')}`);
        }
        const length = header.readUInt32BE(4);
        if (length > MAX_FRAME_LENGTH) {
            throw new Error(`protobuf encoding: frame too large (${length} bytes)`);
        }
        const payload = await reader.readExact(length);
        return this._decodeMessage(payload);
    }

    async writeFrame(send, message) {
        const payload = this._encodeMessage(message);
        if (payload.length > MAX_FRAME_LENGTH) {
            throw new Error(`protobuf encoding: message too large to send (${payload.length} bytes)`);
        }
        const header = Buffer.alloc(8);
        header.writeUInt32BE(FRAME_MAGIC, 0);
        header.writeUInt32BE(payload.length, 4);
        await send(Buffer.concat([header, payload]));
    }

    // --- Encode plain JS  →  binary RSPMessage ---

    _encodeMessage(plainObj) {
        const type = this._registry.getType(RSP_MESSAGE_TYPE);
        // Convert the JSON-compatible object to a form protobufjs can create.
        // service_message in plain JS is { '@type': url, ...fields }; convert to binary Any.
        const prepared = this._prepareForEncode(type, plainObj);
        const err = type.verify(prepared);
        if (err) throw new Error(`ProtobufEncoding encode verify: ${err}`);
        const msg = type.create(prepared);
        return Buffer.from(type.encode(msg).finish());
    }

    _prepareForEncode(type, obj) {
        if (!obj || typeof obj !== 'object' || Array.isArray(obj)) return obj;

        const out = {};
        for (const [key, value] of Object.entries(obj)) {
            if (key === '@type') continue; // handled at service_message level

            const field = type.fields[key];

            // service_message: convert JSON Any to binary Any
            if (key === 'service_message' && value && typeof value === 'object' && value['@type']) {
                out.service_message = this._packAnyFromJsonAny(value);
                continue;
            }

            // Recurse into nested messages
            if (field && field.resolvedType && field.repeated && Array.isArray(value)) {
                out[key] = value.map((item) => this._prepareForEncode(field.resolvedType, item));
            } else if (field && field.resolvedType && value && typeof value === 'object') {
                out[key] = this._prepareForEncode(field.resolvedType, value);
            } else {
                out[key] = value;
            }
        }
        return out;
    }

    _packAnyFromJsonAny(jsonAny) {
        const typeUrl = jsonAny['@type'];
        const typeName = typeUrl.includes('/') ? typeUrl.substring(typeUrl.lastIndexOf('/') + 1) : typeUrl;
        if (!this._registry.hasType(typeName)) {
            // Unknown type: store as empty value with type_url preserved.
            return {type_url: typeUrl, value: Buffer.alloc(0)};
        }
        const innerType = this._registry.getType(typeName);
        const fieldsOnly = Object.fromEntries(Object.entries(jsonAny).filter(([k]) => k !== '@type'));
        const err = innerType.verify(fieldsOnly);
        if (err) throw new Error(`ProtobufEncoding packAny(${typeName}): ${err}`);
        const innerMsg = innerType.create(fieldsOnly);
        return {
            type_url: typeUrl,
            value: Buffer.from(innerType.encode(innerMsg).finish()),
        };
    }

    // --- Decode binary RSPMessage  →  plain JS ---

    _decodeMessage(buf) {
        const type = this._registry.getType(RSP_MESSAGE_TYPE);
        const msg = type.decode(buf);
        const obj = type.toObject(msg, {
            longs: Number,
            bytes: String,     // bytes fields become base64 strings (same as JSON encoding)
            defaults: false,
            arrays: true,
            objects: true,
            oneofs: true,
        });
        // Unpack service_message Any if present
        if (obj.service_message && obj.service_message.type_url) {
            obj.service_message = this._unpackBinaryAny(obj.service_message);
        }
        return obj;
    }

    _unpackBinaryAny(binaryAny) {
        const typeUrl = binaryAny.type_url;
        if (!typeUrl) return binaryAny;

        const typeName = typeUrl.includes('/') ? typeUrl.substring(typeUrl.lastIndexOf('/') + 1) : typeUrl;
        if (!this._registry.hasType(typeName)) {
            // Return a minimal JSON-Any with just the type URL so dispatch can still route on it.
            return {'@type': typeUrl};
        }
        try {
            const innerType = this._registry.getType(typeName);
            const rawValue = binaryAny.value
                ? Buffer.from(binaryAny.value, 'base64')
                : Buffer.alloc(0);
            const innerMsg = innerType.decode(rawValue);
            const innerObj = innerType.toObject(innerMsg, {
                longs: Number,
                bytes: String,
                defaults: false,
            });
            return {'@type': typeUrl, ...innerObj};
        } catch {
            return {'@type': typeUrl};
        }
    }
}

module.exports = {ProtobufEncoding};
