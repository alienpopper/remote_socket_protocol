'use strict';

// RSP full client for Node.js.
//
// Extends the minimal RSPClient with:
//   • Pluggable transports  (default: TcpTransport)
//   • Pluggable encodings   (default: JsonEncoding; also ProtobufEncoding)
//   • ProtoRegistry for dynamic .proto schema loading and message factories
//
// The API is a superset of the minimal nodejs client.  All existing methods
// (ping, connectTCP, listenTCP, acceptTCP, stream*, beginEndorsementRequest,
// queryResources, name*) are inherited unchanged.
//
// Additional API:
//   client.registry               – the live ProtoRegistry
//   client.createServiceMessage(typeName, fields)  – create a typed service_message
//   client.listServiceTypes(filter?)              – list all known protobuf type names
//   client.requestSchema(nodeId, protoFileName?)  – ask a node to send its ServiceSchema
//   client.loadProtoFile(filePath)                – load a .proto file into the registry

const {RSPClient: BaseRSPClient, encodeNodeIdForField} = require('../nodejs/rsp_client');
const {TcpTransport} = require('./transports/tcp');
const {JsonEncoding} = require('./encodings/json');
const {ProtoRegistry} = require('./proto_registry');

const HANDSHAKE_TERMINATOR = Buffer.from('\r\n\r\n', 'ascii');
const TRACE = process.env.RSP_CLIENT_TRACE === '1';

function trace(msg) {
    if (TRACE) console.error(`[rsp_full] ${msg}`);
}

class RSPClient extends BaseRSPClient {
    // options:
    //   transport  – transport plugin instance (default: new TcpTransport())
    //   encoding   – encoding plugin instance  (default: new JsonEncoding())
    //   registry   – ProtoRegistry instance    (default: new ProtoRegistry())
    //   protoRoot  – path to the repo root for loading messages.proto on connect (optional)
    //   ...all options accepted by the base RSPClient constructor
    constructor(keyPair, options = {}) {
        super(keyPair, options);
        this._transport = options.transport || new TcpTransport();
        this._encoding = options.encoding || new JsonEncoding();
        this._registry = options.registry || new ProtoRegistry();
        this._protoRoot = options.protoRoot || null;
        this._rawSend = null;
    }

    // ---- Registry access ----

    get registry() {
        return this._registry;
    }

    // Load a .proto file into the registry.  Call before connecting if you need
    // protobuf encoding, or at any time to add types for use with createServiceMessage.
    async loadProtoFile(filePath) {
        await this._registry.loadProtoFile(filePath);
    }

    // Create a service_message Any object in the format expected by the current encoding.
    // For JSON encoding: { '@type': typeUrl, ...fields }
    // Requires the type to be loaded in the registry (via loadProtoFile or received schema).
    createServiceMessage(typeName, fields = {}) {
        return this._registry.createAny(typeName, fields);
    }

    // List all fully-qualified message type names known to the registry.
    // Optional filter: only names containing the given string are returned.
    listServiceTypes(filter = '') {
        return this._registry.listTypes(filter);
    }

    // Send a SchemaRequest to a node.  The reply arrives as a schema_reply message
    // and is automatically processed by _dispatchMessage to populate the registry.
    async requestSchema(nodeId, protoFileName = null) {
        const fields = {};
        if (protoFileName) fields.proto_file_name = protoFileName;
        await this._sendSignedMessage({
            destination: {value: encodeNodeIdForField(nodeId)},
            schema_request: fields,
        });
    }

    // ---- Overrides: transport + encoding pipeline ----

    async _connectTransport(transportSpec) {
        // Load messages.proto once if a protoRoot was supplied and not yet loaded.
        if (this._protoRoot && !this._registry.isLoaded) {
            const path = require('path');
            const protoFile = path.join(this._protoRoot, 'messages.proto');
            trace(`loading proto: ${protoFile}`);
            await this._registry.loadProtoFile(protoFile).catch((err) => {
                // Non-fatal for JSON encoding; protobuf encoding will fail later if needed.
                trace(`proto load warning: ${err.message}`);
            });
        }

        // Establish the transport connection.
        const {reader, send, socket} = await this._transport.connect(transportSpec);

        // Store handles. this._socket must be the underlying net.Socket so the base
        // class _teardownConnection can call socket.end() / socket.destroy().
        this._socket = socket;
        this._reader = reader;
        this._rawSend = send;

        // ASCII handshake: receive server banner, send our encoding choice, confirm.
        await reader.readUntil(HANDSHAKE_TERMINATOR);
        await send(Buffer.from(`${this._encoding.handshakeToken}\r\n\r\n`, 'ascii'));
        const result = (await reader.readUntil(HANDSHAKE_TERMINATOR)).toString('ascii');
        const expected = `1success: ${this._encoding.handshakeToken}`;
        if (!result.startsWith(expected)) {
            throw new Error(`handshake failed (wanted "${expected}"): ${result.trim()}`);
        }
        trace(`handshake ok: ${this._encoding.handshakeToken}`);

        // Identity exchange and receive loop (same pattern as base class).
        await this._performInitialIdentityExchange();
        this._stopping = false;
        this._receiveLoopPromise = this._receiveLoop().catch((err) => {
            if (!this._stopping) this.emit('error', err);
        });
    }

    async _sendRawMessage(message) {
        await this._ensureConnectedForSend();
        for (let attempt = 0; attempt < 2; attempt++) {
            await this._ensureConnectedForSend();
            const sendFn = this._rawSend;
            try {
                await this._encoding.writeFrame(sendFn, message);
                return;
            } catch (error) {
                const canRetry =
                    this._autoReconnect &&
                    !this._stopping &&
                    attempt === 0 &&
                    error?.message && (
                        error.message.includes('EPIPE') ||
                        error.message.includes('ECONNRESET') ||
                        error.message.includes('socket is closed')
                    );
                if (!canRetry) throw error;
                await this._reconnect('send retry');
            }
        }
    }

    async _receiveRawMessage() {
        if (!this._reader) throw new Error('client is not connected');
        return this._encoding.readFrame(this._reader);
    }

    _dispatchMessage(msg) {
        // Auto-populate the registry from any ServiceSchema payloads that arrive
        // inside ResourceAdvertisements or explicit SchemaReply messages.
        const schemas = msg.resource_advertisement?.schemas || msg.schema_reply?.schemas;
        if (schemas) {
            for (const schema of schemas) {
                try { this._registry.processServiceSchema(schema); } catch { /* ignore */ }
            }
        }
        super._dispatchMessage(msg);
    }
}

module.exports = {
    // Re-export utilities from the base client for convenience.
    // RSPClient from the base is overridden below; spread must come first.
    ...require('../nodejs/rsp_client'),
    // Full client and plugins (these override any same-named base exports).
    RSPClient,
    TcpTransport,
    JsonEncoding,
    ProtoRegistry,
    // Re-export protobuf encoding without creating a hard protobufjs dep at import time.
    get ProtobufEncoding() { return require('./encodings/protobuf').ProtobufEncoding; },
};
