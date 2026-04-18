'use strict';

// ProtoRegistry – dynamic protobuf schema manager for the RSP full client.
//
// Loads .proto files and binary FileDescriptorSets at runtime, building a live
// protobufjs Root that reflects every schema the node has seen.  Provides:
//
//   loadProtoFile(filePath)             – load a .proto file from disk
//   loadDescriptorSet(bytes)            – load a binary FileDescriptorSet (from ServiceSchema)
//   processServiceSchema(schema)        – accept a ServiceSchema object and load its descriptor
//   getType(typeName)                   – look up a protobufjs Type by fully-qualified name
//   create(typeName, fields)            – validate + create a plain-JS message object
//   createAny(typeName, fields)         – like create() but wrapped as a JSON-encoding Any
//   packAny(typeName, fields)           – returns { type_url, value } for protobuf binary Any
//   unpackAny(any)                      – decodes a binary Any { type_url, value } to plain JS
//   listTypes(packageFilter?)           – list all known message type names
//   on('schema_loaded', handler)        – emitted when new types become available

const protobuf = require('protobufjs');
const {EventEmitter} = require('events');

// URL prefix that the RSP system uses for service message type URLs.
const RSP_TYPE_URL_PREFIX = 'type.rsp/';

function typeUrlForName(typeName) {
    return RSP_TYPE_URL_PREFIX + typeName;
}

function typeNameFromUrl(typeUrl) {
    const slash = typeUrl.lastIndexOf('/');
    return slash >= 0 ? typeUrl.substring(slash + 1) : typeUrl;
}

class ProtoRegistry extends EventEmitter {
    constructor() {
        super();
        this._root = new protobuf.Root();
        this._knownHashes = new Set();
        this._loaded = false;
    }

    // Load a .proto source file from the filesystem.
    async loadProtoFile(filePath) {
        await this._root.load(filePath, {keepCase: true, alternateCommentMode: true});
        this._loaded = true;
        this.emit('schema_loaded', {source: filePath});
    }

    // Load a binary-encoded FileDescriptorSet (as Buffer or base64 string) and merge its
    // message types into the live root.  Safe to call multiple times with the same data.
    loadDescriptorSet(bytes) {
        const buf = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes, 'base64');
        // protobufjs decodes a FileDescriptorSet as a namespace Root
        const parsed = protobuf.Root.fromDescriptor(buf);
        this._mergeRoot(parsed);
        this._loaded = true;
        this.emit('schema_loaded', {source: 'descriptor_set'});
    }

    // Process a ServiceSchema object (from ResourceAdvertisement or SchemaReply) and
    // load any new types it describes.  Idempotent: repeated calls with the same hash
    // are ignored.
    processServiceSchema(schema) {
        if (!schema) return;

        // Deduplicate by schema_hash (base64 string) if present.
        const hash = schema.schema_hash;
        if (hash) {
            const key = Buffer.isBuffer(hash) ? hash.toString('base64') : String(hash);
            if (this._knownHashes.has(key)) return;
            this._knownHashes.add(key);
        }

        const descriptor = schema.proto_file_descriptor_set;
        if (descriptor) {
            this.loadDescriptorSet(descriptor);
        }
    }

    // Look up a protobufjs Type by fully-qualified name (e.g. 'rsp.proto.RSPMessage').
    // Throws if the type is not loaded.
    getType(typeName) {
        return this._root.lookupType(typeName);
    }

    // Look up a protobufjs Enum by fully-qualified name.
    getEnum(enumName) {
        return this._root.lookupEnum(enumName);
    }

    // Check whether a type is known without throwing.
    hasType(typeName) {
        try {
            this._root.lookupType(typeName);
            return true;
        } catch {
            return false;
        }
    }

    // Create a plain-JS message object for the given type, validating field names.
    // The returned object is compatible with JSON encoding's service_message format.
    create(typeName, fields = {}) {
        const type = this.getType(typeName);
        const err = type.verify(fields);
        if (err) throw new Error(`ProtoRegistry.create(${typeName}): ${err}`);
        const msg = type.create(fields);
        return type.toObject(msg, {longs: Number, bytes: String, defaults: false, arrays: true, objects: true});
    }

    // Create a service_message Any compatible with JSON encoding:
    //   { '@type': typeUrl, ...fields }
    createAny(typeName, fields = {}) {
        return {'@type': typeUrlForName(typeName), ...this.create(typeName, fields)};
    }

    // Create a service_message Any compatible with protobuf binary encoding:
    //   { type_url: string, value: Buffer }
    packAny(typeName, fields = {}) {
        const type = this.getType(typeName);
        const err = type.verify(fields);
        if (err) throw new Error(`ProtoRegistry.packAny(${typeName}): ${err}`);
        const msg = type.create(fields);
        return {
            type_url: typeUrlForName(typeName),
            value: Buffer.from(type.encode(msg).finish()),
        };
    }

    // Decode a binary Any { type_url, value } to a plain-JS { typeName, message } object.
    // Returns null if the type is not loaded.
    unpackAny(any) {
        if (!any || !any.type_url) return null;
        const typeName = typeNameFromUrl(any.type_url);
        if (!this.hasType(typeName)) return null;
        try {
            const type = this.getType(typeName);
            const value = Buffer.isBuffer(any.value) ? any.value : Buffer.from(any.value || '');
            const msg = type.decode(value);
            return {
                typeName,
                message: type.toObject(msg, {longs: Number, bytes: String, defaults: false}),
            };
        } catch {
            return null;
        }
    }

    // Return all fully-qualified message type names currently known to the registry.
    // Optionally filter to names containing the packageFilter string.
    listTypes(packageFilter = '') {
        const names = [];
        this._collectTypes(this._root, '', names);
        if (!packageFilter) return names;
        return names.filter((n) => n.includes(packageFilter));
    }

    // Expose the underlying protobufjs Root for advanced usage.
    get root() {
        return this._root;
    }

    get isLoaded() {
        return this._loaded;
    }

    // ---- Private ----

    _mergeRoot(parsed) {
        // Walk all nested namespaces of the parsed root and graft them into this._root.
        for (const nestedName of Object.keys(parsed.nested || {})) {
            const nestedObj = parsed.nested[nestedName];
            const existing = this._root.nested && this._root.nested[nestedName];
            if (!existing) {
                this._root.add(nestedObj);
            } else if (nestedObj instanceof protobuf.Namespace) {
                this._mergeNamespace(existing, nestedObj);
            }
            // Scalar type conflicts are silently ignored (same type, different instance).
        }
    }

    _mergeNamespace(target, source) {
        for (const nestedName of Object.keys(source.nested || {})) {
            const child = source.nested[nestedName];
            const existing = target.nested && target.nested[nestedName];
            if (!existing) {
                target.add(child);
            } else if (child instanceof protobuf.Namespace && existing instanceof protobuf.Namespace) {
                this._mergeNamespace(existing, child);
            }
        }
    }

    _collectTypes(ns, prefix, names) {
        if (!ns || !ns.nested) return;
        for (const [name, obj] of Object.entries(ns.nested)) {
            const fullName = prefix ? `${prefix}.${name}` : name;
            if (obj instanceof protobuf.Type) {
                names.push(fullName);
            }
            if (obj instanceof protobuf.Namespace) {
                this._collectTypes(obj, fullName, names);
            }
        }
    }
}

module.exports = {ProtoRegistry, typeUrlForName, typeNameFromUrl};
