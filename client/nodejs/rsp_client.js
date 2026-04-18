'use strict';

const crypto = require('crypto');
const net = require('net');
const os = require('os');
const {EventEmitter, once} = require('events');
const messages = require('./messages');

const JSON_FRAME_MAGIC = 0x5253504a;
const HANDSHAKE_TERMINATOR = Buffer.from('\r\n\r\n', 'ascii');
const P256_ALGORITHM = 100;
const DEFAULT_TIMEOUT_MS = 5000;

const {
    SUCCESS,
    CONNECT_REFUSED,
    CONNECT_TIMEOUT,
    STREAM_CLOSED,
    STREAM_DATA,
    STREAM_ERROR,
    NEW_CONNECTION,
    ASYNC_STREAM,
    INVALID_FLAGS,
    STREAM_IN_USE,
} = messages.STREAM_STATUS;

const {
    ENDORSEMENT_SUCCESS,
    ENDORSEMENT_UNKNOWN_IDENTITY,
} = messages.ENSDORSMENT_STATUS;

const TRACE = process.env.RSP_CLIENT_TRACE === '1';

function trace(message) {
    if (TRACE) {
        console.error(`[rsp_client] ${message}`);
    }
}

function normalizeGuid(value) {
    const compact = String(value).replace(/[{}-]/g, '').toLowerCase();
    if (!/^[0-9a-f]{32}$/.test(compact)) {
        throw new Error(`invalid GUID/NodeID: ${value}`);
    }
    return compact;
}

function guidFromBytes(bytes) {
    if (!Buffer.isBuffer(bytes) || bytes.length !== 16) {
        throw new Error('GUID bytes must be exactly 16 bytes');
    }

    const hex = bytes.toString('hex');
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20, 32)}`;
}

function parseGuid(value) {
    const normalized = normalizeGuid(value);
    return {
        normalized,
        high: BigInt(`0x${normalized.slice(0, 16)}`),
        low: BigInt(`0x${normalized.slice(16, 32)}`),
    };
}

function encodeNodeIdForField(nodeId) {
    const parsed = parseGuid(nodeId);
    const bytes = Buffer.alloc(16);
    if (os.endianness() === 'LE') {
        bytes.writeBigUInt64LE(parsed.high, 0);
        bytes.writeBigUInt64LE(parsed.low, 8);
    } else {
        bytes.writeBigUInt64BE(parsed.high, 0);
        bytes.writeBigUInt64BE(parsed.low, 8);
    }
    return bytes.toString('base64');
}

function decodeNodeIdField(b64Value) {
    const bytes = Buffer.from(b64Value, 'base64');
    if (bytes.length !== 16) {
        throw new Error('NodeId field must be 16 bytes');
    }

    const high = os.endianness() === 'LE' ? bytes.readBigUInt64LE(0) : bytes.readBigUInt64BE(0);
    const low = os.endianness() === 'LE' ? bytes.readBigUInt64LE(8) : bytes.readBigUInt64BE(8);
    const canonical = Buffer.alloc(16);
    canonical.writeBigUInt64BE(high, 0);
    canonical.writeBigUInt64BE(low, 8);
    return guidFromBytes(canonical);
}

function encodeNodeIdForSigner(nodeId) {
    const parsed = parseGuid(nodeId);
    const bytes = Buffer.alloc(16);
    bytes.writeBigUInt64BE(parsed.high, 0);
    bytes.writeBigUInt64BE(parsed.low, 8);
    return bytes.toString('base64');
}

function decodeSignerNodeId(b64Value) {
    return guidFromBytes(Buffer.from(b64Value, 'base64'));
}

function randomUuidB64() {
    return crypto.randomBytes(16).toString('base64');
}

function publicKeyPemToDer(publicKeyPem) {
    return crypto.createPublicKey(publicKeyPem).export({type: 'spki', format: 'der'});
}

function nodeIdFromPublicKeyPem(publicKeyPem) {
    const digest = crypto.createHash('sha256').update(publicKeyPemToDer(publicKeyPem)).digest();
    return guidFromBytes(digest.subarray(0, 16));
}

function hasField(object, key) {
    return object && Object.prototype.hasOwnProperty.call(object, key) && object[key] !== undefined && object[key] !== null;
}

const SERVICE_MESSAGE_TYPE_PREFIX = 'type.rsp/rsp.proto.';

function packServiceMessage(typeName, fields) {
    return {'@type': SERVICE_MESSAGE_TYPE_PREFIX + typeName, ...fields};
}

function serviceMessageTypeName(msg) {
    if (!hasField(msg, 'service_message') || !msg.service_message['@type']) return null;
    const typeUrl = msg.service_message['@type'];
    const slash = typeUrl.lastIndexOf('/');
    return slash >= 0 ? typeUrl.substring(slash + 1) : typeUrl;
}

function unpackServiceMessage(msg) {
    if (!hasField(msg, 'service_message')) return null;
    const copy = {...msg.service_message};
    delete copy['@type'];
    return copy;
}

// --- Signing ---
// All signing uses the canonical hash from messages.js — never raw protobuf bytes.
// Signing raw protobuf would require a protobuf dependency in clients, which is a bug.

function signRSPMessage(privateKeyPem, localNodeId, message) {
    const digest = messages.hashRSPMessage(message);
    const signer = crypto.createSign('sha256');
    signer.update(digest);
    signer.end();
    return {
        signer: {value: encodeNodeIdForSigner(localNodeId)},
        algorithm: P256_ALGORITHM,
        signature: signer.sign(privateKeyPem).toString('base64'),
    };
}

function verifyRSPMessageSignature(publicKeyPem, message, signatureBlock) {
    if (!signatureBlock || signatureBlock.algorithm !== P256_ALGORITHM) {
        return false;
    }
    if (decodeSignerNodeId(signatureBlock.signer.value) !== nodeIdFromPublicKeyPem(publicKeyPem)) {
        return false;
    }
    const digest = messages.hashRSPMessage(message);
    const verifier = crypto.createVerify('sha256');
    verifier.update(digest);
    verifier.end();
    return verifier.verify(publicKeyPem, Buffer.from(signatureBlock.signature, 'base64'));
}

function encodeVarintUnsigned(value) {
    let remaining = BigInt(value);
    if (remaining < 0n) {
        throw new Error('varint value must be non-negative');
    }
    const bytes = [];
    do {
        let byte = Number(remaining & 0x7Fn);
        remaining >>= 7n;
        if (remaining !== 0n) {
            byte |= 0x80;
        }
        bytes.push(byte);
    } while (remaining !== 0n);
    return Buffer.from(bytes);
}

function encodeTag(fieldNumber, wireType) {
    return encodeVarintUnsigned((BigInt(fieldNumber) << 3n) | BigInt(wireType));
}

function encodeLengthDelimitedField(fieldNumber, payload) {
    return Buffer.concat([
        encodeTag(fieldNumber, 2),
        encodeVarintUnsigned(payload.length),
        payload,
    ]);
}

function serializeUuidLike(b64Value) {
    return encodeLengthDelimitedField(1, Buffer.from(b64Value, 'base64'));
}

function serializeDateTimeMessage(millisecondsSinceEpoch) {
    return Buffer.concat([
        encodeTag(1, 0),
        encodeVarintUnsigned(BigInt(millisecondsSinceEpoch)),
    ]);
}

function serializeUnsignedEndorsement(endorsement) {
    return Buffer.concat([
        encodeLengthDelimitedField(1, serializeUuidLike(endorsement.subject.value)),
        encodeLengthDelimitedField(2, serializeUuidLike(endorsement.endorsement_service.value)),
        encodeLengthDelimitedField(3, serializeUuidLike(endorsement.endorsement_type.value)),
        encodeLengthDelimitedField(4, Buffer.from(endorsement.endorsement_value || '', 'base64')),
        encodeLengthDelimitedField(5, serializeDateTimeMessage(endorsement.valid_until.milliseconds_since_epoch)),
    ]);
}

function signEndorsement(privateKeyPem, localNodeId, endorsement) {
    const _unused = localNodeId;
    const unsignedBytes = serializeUnsignedEndorsement(endorsement);
    const signer = crypto.createSign('sha256');
    signer.update(unsignedBytes);
    signer.end();
    return signer.sign(privateKeyPem).toString('base64');
}

// --- Transport utilities ---

class BufferedSocketReader {
    constructor(socket) {
        this.socket = socket;
        this.buffer = Buffer.alloc(0);
        this.ended = false;
        this.error = null;
        this.events = new EventEmitter();

        socket.on('data', (chunk) => {
            this.buffer = Buffer.concat([this.buffer, chunk]);
            this.events.emit('update');
        });
        socket.on('end', () => { this.ended = true; this.events.emit('update'); });
        socket.on('close', () => { this.ended = true; this.events.emit('update'); });
        socket.on('error', (error) => { this.error = error; this.events.emit('update'); });
    }

    async waitForData() {
        if (this.error) throw this.error;
        if (this.ended) throw new Error('socket closed while waiting for data');
        await once(this.events, 'update');
    }

    async readUntil(marker) {
        while (true) {
            const index = this.buffer.indexOf(marker);
            if (index >= 0) {
                const chunk = this.buffer.subarray(0, index + marker.length);
                this.buffer = this.buffer.subarray(index + marker.length);
                return chunk;
            }
            await this.waitForData();
        }
    }

    async readExact(length) {
        while (this.buffer.length < length) {
            await this.waitForData();
        }
        const chunk = this.buffer.subarray(0, length);
        this.buffer = this.buffer.subarray(length);
        return chunk;
    }
}

function parseTransportSpec(transportSpec) {
    const value = String(transportSpec);
    const separator = value.indexOf(':');
    if (separator <= 0 || separator + 1 >= value.length) {
        throw new Error('transport must be in the format <name>:<parameters>');
    }

    const transportName = value.slice(0, separator);
    const parameters = value.slice(separator + 1);
    if (transportName !== 'tcp') {
        throw new Error('the Node.js client currently supports only tcp transport');
    }

    const endpointSeparator = parameters.lastIndexOf(':');
    if (endpointSeparator <= 0 || endpointSeparator + 1 >= parameters.length) {
        throw new Error('tcp transport must be in the format tcp:<host>:<port>');
    }

    return {
        host: parameters.slice(0, endpointSeparator),
        port: Number.parseInt(parameters.slice(endpointSeparator + 1), 10),
    };
}

// --- RSPClient ---
// Mirrors the API of the C++ RSPClient. Socket operation options are Node.js-idiomatic
// objects rather than positional parameters.

class RSPClient extends EventEmitter {
    constructor(keyPair, options = {}) {
        super();
        const generated = keyPair || crypto.generateKeyPairSync('ec', {
            namedCurve: 'prime256v1',
            privateKeyEncoding: {type: 'pkcs8', format: 'pem'},
            publicKeyEncoding: {type: 'spki', format: 'pem'},
        });

        this._privateKeyPem = generated.privateKey;
        this._publicKeyPem = generated.publicKey;
        this.nodeId = nodeIdFromPublicKeyPem(this._publicKeyPem);

        this._socket = null;
        this._reader = null;
        this._stopping = false;
        this._receiveLoopPromise = null;
        this._transportSpec = null;
        this._reconnectPromise = null;

        this._autoReconnect = false;
        this._autoReconnectInitialDelayMs = 250;
        this._autoReconnectMaxDelayMs = 3000;

        this.peerNodeId = null;
        this.peerPublicKeyPem = null;

        this._pingSequence = 1;
        this._pendingPings = new Map();        // nonce -> {sequence, resolve, reject, timer}

        this._pendingConnects = new Map();     // socketId -> {resolve, reject, timer}
        this._pendingListens = new Map();      // socketId -> {resolve, reject, timer}
        this._streamRoutes = new Map();        // socketId -> nodeId
        this._streamReplyQueues = new Map();   // socketId -> StreamReply[]
        this._awaitedStreamReplies = new Map();// socketId -> {resolve, reject, timer}
        this._pendingStreamReplies = [];       // global async queue

        this._pendingEndorsements = new Map(); // nodeIdHex -> {resolve, reject, timer}
        this._endorsementCache = new Map();    // `${nodeId}:${typeHex}` -> endorsement

        this._pendingResourceAdvertisements = [];
        this._pendingResourceQueryReplies = [];
        this._pendingResourceList = null;       // {resolve, reject, timer}
        this._pendingNameReply = null;          // {resolve, reject, timer}
        this._identityCache = new Map();       // nodeId -> publicKeyPem

        this._streamHandlers = new Map();      // socketId -> handler (used by rsp_net.js)

        this._configureAutoReconnect(options.autoReconnect);
    }

    // --- Connection lifecycle ---

    _configureAutoReconnect(autoReconnectOptions) {
        if (autoReconnectOptions === undefined) {
            return;
        }
        if (typeof autoReconnectOptions === 'boolean') {
            this._autoReconnect = autoReconnectOptions;
            return;
        }
        if (typeof autoReconnectOptions !== 'object' || autoReconnectOptions === null) {
            throw new Error('autoReconnect must be a boolean or options object');
        }

        this._autoReconnect = autoReconnectOptions.enabled !== false;
        if (Number.isFinite(autoReconnectOptions.initialDelayMs) && autoReconnectOptions.initialDelayMs >= 0) {
            this._autoReconnectInitialDelayMs = Math.floor(autoReconnectOptions.initialDelayMs);
        }
        if (Number.isFinite(autoReconnectOptions.maxDelayMs) && autoReconnectOptions.maxDelayMs >= 0) {
            this._autoReconnectMaxDelayMs = Math.floor(autoReconnectOptions.maxDelayMs);
        }
        if (this._autoReconnectMaxDelayMs < this._autoReconnectInitialDelayMs) {
            this._autoReconnectMaxDelayMs = this._autoReconnectInitialDelayMs;
        }
    }

    async connect(transportSpec, options = {}) {
        this._configureAutoReconnect(options.autoReconnect);
        this._transportSpec = transportSpec;
        this._stopping = false;
        await this._connectTransport(transportSpec);
    }

    async _connectTransport(transportSpec) {
        const endpoint = parseTransportSpec(transportSpec);
        const socket = net.createConnection(endpoint);
        socket.setNoDelay(true);
        await once(socket, 'connect');

        this._socket = socket;
        this._reader = new BufferedSocketReader(socket);

        await this._reader.readUntil(HANDSHAKE_TERMINATOR);
        socket.write('encoding:json\r\n\r\n', 'ascii');

        const result = (await this._reader.readUntil(HANDSHAKE_TERMINATOR)).toString('ascii');
        if (!result.startsWith('1success: encoding:json')) {
            throw new Error(`ASCII handshake failed: ${result.trim()}`);
        }

        await this._performInitialIdentityExchange();

        this._stopping = false;
        this._receiveLoopPromise = this._receiveLoop().catch((err) => {
            if (!this._stopping) this.emit('error', err);
        });
    }

    async close() {
        this._stopping = true;
        this._autoReconnect = false;

        const reconnectPromise = this._reconnectPromise;
        this._reconnectPromise = null;

        this._teardownConnection('client closed');

        if (reconnectPromise) {
            await reconnectPromise.catch(() => {});
        }

        const receiveLoopPromise = this._receiveLoopPromise;
        this._receiveLoopPromise = null;
        if (receiveLoopPromise) {
            await receiveLoopPromise.catch(() => {});
        }
    }

    _rejectAllPending(errorMessage) {
        const closedError = new Error(errorMessage);
        for (const [, p] of this._pendingPings) { clearTimeout(p.timer); p.reject(closedError); }
        this._pendingPings.clear();
        for (const [, p] of this._pendingConnects) { clearTimeout(p.timer); p.reject(closedError); }
        this._pendingConnects.clear();
        for (const [, p] of this._pendingListens) { clearTimeout(p.timer); p.reject(closedError); }
        this._pendingListens.clear();
        for (const [, p] of this._awaitedStreamReplies) { clearTimeout(p.timer); p.reject(closedError); }
        this._awaitedStreamReplies.clear();
        for (const [, p] of this._pendingEndorsements) { clearTimeout(p.timer); p.reject(closedError); }
        this._pendingEndorsements.clear();
    }

    _teardownConnection(reason) {
        const socket = this._socket;
        this._socket = null;
        this._reader = null;
        this.peerNodeId = null;
        this.peerPublicKeyPem = null;

        this._rejectAllPending(reason);

        this._streamHandlers.clear();
        this._streamRoutes.clear();
        this._streamReplyQueues.clear();
        this._pendingStreamReplies = [];

        if (!socket) {
            return;
        }

        if (!socket.destroyed) {
            socket.end();
            setTimeout(() => {
                if (!socket.destroyed) {
                    socket.destroy();
                }
            }, 250);
        }
    }

    async _ensureConnectedForSend() {
        if (this._socket) {
            return;
        }
        if (!this._autoReconnect || !this._transportSpec || this._stopping) {
            throw new Error('client is not connected');
        }
        await this._reconnect('send path');
        if (!this._socket) {
            throw new Error('client is not connected');
        }
    }

    async _reconnect(trigger) {
        if (this._reconnectPromise) {
            return this._reconnectPromise;
        }
        this._reconnectPromise = (async () => {
            if (!this._autoReconnect || !this._transportSpec || this._stopping) {
                return;
            }

            this._teardownConnection('connection lost during reconnect');
            this.emit('reconnecting', trigger);

            let delayMs = this._autoReconnectInitialDelayMs;
            while (!this._stopping && this._autoReconnect) {
                try {
                    await this._connectTransport(this._transportSpec);
                    this.emit('reconnected');
                    return;
                } catch (error) {
                    this.emit('reconnect_attempt_failed', error);
                    await new Promise((resolve) => setTimeout(resolve, delayMs));
                    delayMs = Math.min(delayMs * 2, this._autoReconnectMaxDelayMs);
                }
            }
        })();

        try {
            await this._reconnectPromise;
        } finally {
            this._reconnectPromise = null;
        }
    }

    // --- Identity exchange ---

    async _performInitialIdentityExchange() {
        const localChallengeNonce = randomUuidB64();
        await this._sendRawMessage({challenge_request: {nonce: {value: localChallengeNonce}}});

        let peerChallengeReceived = false;
        let peerIdentityReceived = false;

        while (!peerChallengeReceived || !peerIdentityReceived) {
            const message = await this._receiveRawMessage();

            if (hasField(message, 'challenge_request')) {
                const nonceHex = message.challenge_request?.nonce?.value;
                if (message.destination || message.signature || peerChallengeReceived ||
                    !nonceHex) {
                    throw new Error('received an invalid challenge request during authentication');
                }
                const identityMessage = {
                    identity: {
                        nonce: {value: nonceHex},
                        public_key: {
                            algorithm: P256_ALGORITHM,
                            public_key: Buffer.from(this._publicKeyPem, 'utf8').toString('base64'),
                        },
                    },
                };
                identityMessage.signature = signRSPMessage(this._privateKeyPem, this.nodeId, identityMessage);
                await this._sendRawMessage(identityMessage);
                peerChallengeReceived = true;
                continue;
            }

            if (hasField(message, 'identity')) {
                const peerPublicKeyPem = Buffer.from(message.identity.public_key.public_key, 'base64').toString('utf8');
                if (message.identity?.nonce?.value !== localChallengeNonce ||
                    !verifyRSPMessageSignature(peerPublicKeyPem, message, message.signature)) {
                    throw new Error('received an invalid identity response during authentication');
                }
                this.peerPublicKeyPem = peerPublicKeyPem;
                this.peerNodeId = nodeIdFromPublicKeyPem(peerPublicKeyPem);
                this._identityCache.set(this.peerNodeId, peerPublicKeyPem);
                peerIdentityReceived = true;
                continue;
            }

            throw new Error('received an unexpected message during authentication');
        }
    }

    async _sendIdentityTo(nodeId) {
        const identityMessage = {
            destination: {value: encodeNodeIdForField(nodeId)},
            identities: [{
                public_key: {
                    algorithm: P256_ALGORITHM,
                    public_key: Buffer.from(this._publicKeyPem, 'utf8').toString('base64'),
                },
            }],
        };
        identityMessage.signature = signRSPMessage(this._privateKeyPem, this.nodeId, identityMessage);
        await this._sendRawMessage(identityMessage);
    }

    // --- Receive loop ---

    async _receiveLoop() {
        while (!this._stopping && this._reader) {
            let message;
            try {
                message = await this._receiveRawMessage();
            } catch (error) {
                const transportClosed =
                    !this._socket ||
                    (this._socket && (!this._socket.readable || this._socket.destroyed)) ||
                    (error && typeof error.message === 'string' && (
                        error.message.includes('socket closed while waiting for data') ||
                        error.message.includes('client is not connected') ||
                        error.message.includes('ECONNRESET') ||
                        error.message.includes('EPIPE')
                    ));

                if (!this._stopping && transportClosed && this._autoReconnect) {
                    await this._reconnect('receive loop');
                } else if (!this._stopping && !transportClosed) {
                    throw new Error('receive loop ended unexpectedly');
                }
                return;
            }
            try {
                this._dispatchMessage(message);
            } catch (err) {
                this.emit('error', err);
            }
        }
    }

    _dispatchMessage(msg) {
        if (Array.isArray(msg.endorsements)) {
            for (const endorsement of msg.endorsements) {
                this._cacheEndorsement(endorsement);
            }
        }

        if (Array.isArray(msg.identities)) {
            for (const identity of msg.identities) {
                this._cacheIdentity(msg, identity);
            }
        }

        if (hasField(msg, 'ping_reply')) {
            this._handlePingReply(msg);
        } else if (hasField(msg, 'service_message')) {
            const typeName = serviceMessageTypeName(msg);
            if (typeName === 'rsp.proto.StreamReply') {
                this._handleStreamReply(msg, unpackServiceMessage(msg));
            } else if (typeName === 'rsp.proto.EndorsementDone') {
                this._handleEndorsementDone(msg);
            } else if (typeName && typeName.startsWith('rsp.proto.Name') && typeName.endsWith('Reply')) {
                this._handleNameReply(msg);
            } else {
                trace(`unhandled service_message type=${typeName}`);
            }
        } else if (hasField(msg, 'endorsement_needed')) {
            trace('received endorsement_needed');
            this.emit('endorsement_needed', msg.endorsement_needed);
        } else if (hasField(msg, 'resource_advertisement')) {
            trace('received resource_advertisement');
            this._pendingResourceAdvertisements.push(msg.resource_advertisement);
            this.emit('resource_advertisement', msg.resource_advertisement);
        } else if (hasField(msg, 'resource_query_reply')) {
            trace('received resource_query_reply');
            if (this._pendingResourceList) {
                const {resolve, timer} = this._pendingResourceList;
                this._pendingResourceList = null;
                clearTimeout(timer);
                resolve(msg.resource_query_reply);
            } else {
                this._pendingResourceQueryReplies.push(msg.resource_query_reply);
            }
            this.emit('resource_query_reply', msg.resource_query_reply);
        } else {
            const keys = Object.keys(msg || {}).join(',');
            if (hasField(msg, 'error')) {
                trace(`unhandled message error=${msg.error} keys=${keys}`);
            } else if (hasField(msg, 'endorsement_needed')) {
                trace(`endorsement_needed keys=${keys}`);
            } else {
                trace(`unhandled message keys=${keys}`);
            }
        }
    }

    _handlePingReply(msg) {
        const reply = msg.ping_reply;
        const nonce = reply?.nonce?.value;
        if (!nonce) return;
        const pending = this._pendingPings.get(nonce);
        if (!pending || reply.sequence !== pending.sequence) return;
        clearTimeout(pending.timer);
        this._pendingPings.delete(nonce);
        pending.resolve(true);
    }

    _handleStreamReply(msg, streamReply) {
        const streamIdHex = streamReply.stream_id?.value;
        const status = streamReply.error || 0;
        if (streamIdHex) {
            trace(`socket_reply socket=${streamIdHex} status=${status}`);
        }

        if (streamIdHex) {
            const connectPending = this._pendingConnects.get(streamIdHex);
            if (connectPending) {
                const status = streamReply.error || 0;
                if (status === SUCCESS || status === CONNECT_REFUSED || status === CONNECT_TIMEOUT ||
                    status === STREAM_ERROR || status === STREAM_IN_USE || status === INVALID_FLAGS) {
                    clearTimeout(connectPending.timer);
                    this._pendingConnects.delete(streamIdHex);
                    connectPending.resolve(streamReply);
                    return;
                }
            }

            const listenPending = this._pendingListens.get(streamIdHex);
            if (listenPending) {
                const status = streamReply.error || 0;
                if (status === SUCCESS || status === STREAM_ERROR ||
                    status === STREAM_IN_USE || status === INVALID_FLAGS) {
                    clearTimeout(listenPending.timer);
                    this._pendingListens.delete(streamIdHex);
                    listenPending.resolve(streamReply);
                    return;
                }
            }

            if (streamReply.new_stream_id?.value) {
                const sourceNodeId = this._senderNodeIdFromMessage(msg);
                if (sourceNodeId) {
                    this._streamRoutes.set(streamReply.new_stream_id.value, sourceNodeId);
                }
            }

            const awaited = this._awaitedStreamReplies.get(streamIdHex);
            if (awaited && (!awaited.predicate || awaited.predicate(streamReply))) {
                this._awaitedStreamReplies.delete(streamIdHex);
                clearTimeout(awaited.timer);
                awaited.resolve(streamReply);
                return;
            }

            const handler = this._streamHandlers.get(streamIdHex);
            if (handler) {
                handler(streamReply);
                return;
            }

            const queue = this._streamReplyQueues.get(streamIdHex) || [];
            queue.push(streamReply);
            this._streamReplyQueues.set(streamIdHex, queue);
        }

        this._pendingStreamReplies.push(streamReply);
        this.emit('stream_reply', streamIdHex, streamReply);
    }

    _handleEndorsementDone(msg) {
        const sourceNodeId = this._senderNodeIdFromMessage(msg);
        if (!sourceNodeId) return;
        const pendingKey = encodeNodeIdForField(sourceNodeId);
        const pending = this._pendingEndorsements.get(pendingKey);
        if (!pending) return;
        clearTimeout(pending.timer);
        this._pendingEndorsements.delete(pendingKey);
        pending.resolve(unpackServiceMessage(msg));
    }

    _senderNodeIdFromMessage(msg) {
        if (msg.source?.value) {
            try {
                return decodeNodeIdField(msg.source.value);
            } catch {
                // Fall through to signature signer when source encoding is absent or malformed.
            }
        }

        const signerHex = msg.signature?.signer?.value;
        if (!signerHex) return null;
        try {
            return decodeSignerNodeId(signerHex);
        } catch {
            return null;
        }
    }

    _cacheEndorsement(endorsement) {
        const subjectHex = endorsement.subject?.value;
        const typeHex = endorsement.endorsement_type?.value;
        if (!subjectHex || !typeHex) return;
        try {
            const subject = decodeNodeIdField(subjectHex);
            this._endorsementCache.set(`${subject}:${typeHex}`, endorsement);
        } catch {
            // ignore malformed endorsements
        }
    }

    _cacheIdentity(msg, identity) {
        if (!identity?.public_key?.public_key) return;
        try {
            const pem = Buffer.from(identity.public_key.public_key, 'base64').toString('utf8');
            const nodeId = nodeIdFromPublicKeyPem(pem);
            this._identityCache.set(nodeId, pem);
        } catch {
            // ignore malformed identities
        }
    }

    // --- Message send/receive primitives ---

    async _sendRawMessage(message) {
        await this._ensureConnectedForSend();
        if (TRACE) {
            const keys = Object.keys(message || {}).join(',');
            const svcType = serviceMessageTypeName(message);
            if (svcType === 'rsp.proto.SocketSend') {
                const sm = unpackServiceMessage(message);
                const socketHex = sm?.socket_number?.value || 'none';
                const bytes = sm?.data ? Buffer.from(sm.data, 'base64').length : 0;
                trace(`send_raw keys=${keys} socket_send.socket=${socketHex} bytes=${bytes}`);
            }
        }
        const payload = Buffer.from(JSON.stringify(message), 'utf8');
        const header = Buffer.alloc(8);
        header.writeUInt32BE(JSON_FRAME_MAGIC, 0);
        header.writeUInt32BE(payload.length, 4);

        const frame = Buffer.concat([header, payload]);
        for (let attempt = 0; attempt < 2; attempt += 1) {
            await this._ensureConnectedForSend();
            const targetSocket = this._socket;
            try {
                await new Promise((resolve, reject) => {
                    targetSocket.write(frame, (err) => err ? reject(err) : resolve());
                });
                return;
            } catch (error) {
                const canRetry =
                    this._autoReconnect &&
                    !this._stopping &&
                    attempt === 0 &&
                    error && typeof error.message === 'string' && (
                        error.message.includes('EPIPE') ||
                        error.message.includes('ECONNRESET') ||
                        error.message.includes('socket is closed')
                    );
                if (!canRetry) {
                    throw error;
                }
                await this._reconnect('send retry');
            }
        }
    }

    async _receiveRawMessage() {
        if (!this._reader) throw new Error('client is not connected');
        const header = await this._reader.readExact(8);
        const magic = header.readUInt32BE(0);
        if (magic !== JSON_FRAME_MAGIC) {
            throw new Error(`unexpected JSON frame magic: 0x${magic.toString(16)}`);
        }
        const payloadLength = header.readUInt32BE(4);
        const payload = await this._reader.readExact(payloadLength);
        const parsed = JSON.parse(payload.toString('utf8'));
        if (TRACE) {
            const keys = Object.keys(parsed || {}).join(',');
            if (hasField(parsed, 'stream_reply') || hasField(parsed, 'error') || hasField(parsed, 'endorsement_needed')) {
                trace(`recv_raw keys=${keys}`);
            }
        }
        return parsed;
    }

    async _sendSignedMessage(message) {
        const signed = Object.assign({}, message, {
            signature: signRSPMessage(this._privateKeyPem, this.nodeId, message),
        });
        await this._sendRawMessage(signed);
    }

    // --- Socket reply waiting ---

    _waitForStreamReply(socketId, timeoutMs = DEFAULT_TIMEOUT_MS, predicate = null) {
        const queue = this._streamReplyQueues.get(socketId);
        if (queue && queue.length > 0) {
            const index = predicate ? queue.findIndex(predicate) : 0;
            if (index >= 0) {
                const [reply] = queue.splice(index, 1);
                if (queue.length === 0) this._streamReplyQueues.delete(socketId);
                const idx = this._pendingStreamReplies.indexOf(reply);
                if (idx >= 0) this._pendingStreamReplies.splice(idx, 1);
                return Promise.resolve(reply);
            }
        }
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._awaitedStreamReplies.delete(socketId);
                reject(new Error(`socket reply timed out for socket ${socketId}`));
            }, timeoutMs);
            this._awaitedStreamReplies.set(socketId, {resolve, reject, timer, predicate});
        });
    }

    // --- Ping ---

    async ping(nodeId, timeoutMs = DEFAULT_TIMEOUT_MS) {
        const pingNonce = randomUuidB64();
        const sequence = this._pingSequence++;
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            nonce: {value: randomUuidB64()},
            ping_request: {
                nonce: {value: pingNonce},
                sequence,
                time_sent: {milliseconds_since_epoch: Date.now()},
            },
        };
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pendingPings.delete(pingNonce);
                resolve(false);
            }, timeoutMs);
            this._pendingPings.set(pingNonce, {sequence, resolve, reject, timer});
            this._sendSignedMessage(request).catch((err) => {
                clearTimeout(timer);
                this._pendingPings.delete(pingNonce);
                reject(err);
            });
        });
    }

    // --- Connect ---

    async connectTCPEx(nodeId, hostPort, options = {}) {
        const {
            timeoutMs = 0, retries = 0, retryMs = 0,
            asyncData = false, shareSocket = false, useSocket = false,
        } = options;

        const socketId = randomUuidB64();
        const connectFields = {
            host_port: hostPort,
            stream_id: {value: socketId},
        };
        if (useSocket) connectFields.use_socket = true;
        if (timeoutMs > 0) connectFields.timeout_ms = timeoutMs;
        if (retries > 0) connectFields.retries = retries;
        if (retryMs > 0) connectFields.retry_ms = retryMs;
        if (asyncData) connectFields.async_data = true;
        if (shareSocket) connectFields.share_socket = true;
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            service_message: packServiceMessage('ConnectTCPRequest', connectFields),
        };

        const waitMs = timeoutMs > 0 ? timeoutMs + 1000 : DEFAULT_TIMEOUT_MS;
        const reply = await new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pendingConnects.delete(socketId);
                reject(new Error('connectTCP timed out'));
            }, waitMs);
            this._pendingConnects.set(socketId, {resolve, reject, timer});
            this._sendSignedMessage(request).catch((err) => {
                clearTimeout(timer);
                this._pendingConnects.delete(socketId);
                reject(err);
            });
        });

        if (reply && (reply.error || 0) === SUCCESS) {
            const confirmedId = reply.stream_id?.value || socketId;
            if (!reply.stream_id?.value) reply.socket_id = {value: socketId};
            this._streamRoutes.set(confirmedId, nodeId);
        }
        return reply;
    }

    async connectTCP(nodeId, hostPort, options = {}) {
        let reply;
        try { reply = await this.connectTCPEx(nodeId, hostPort, options); } catch { return null; }
        if (!reply || (reply.error || 0) !== SUCCESS || !reply.stream_id?.value) return null;
        return reply.stream_id.value;
    }

    // --- Listen ---

    async listenTCPEx(nodeId, hostPort, options = {}) {
        const {
            timeoutMs = 0, asyncAccept = false, shareListeningSocket = false,
            shareChildSockets = false, childrenUseSocket = false, childrenAsyncData = false,
        } = options;

        const socketId = randomUuidB64();
        const listenFields = {
            host_port: hostPort,
            stream_id: {value: socketId},
        };
        if (timeoutMs > 0) listenFields.timeout_ms = timeoutMs;
        if (asyncAccept) listenFields.async_accept = true;
        if (shareListeningSocket) listenFields.share_listening_socket = true;
        if (shareChildSockets) listenFields.share_child_sockets = true;
        if (childrenUseSocket) listenFields.children_use_socket = true;
        if (childrenAsyncData) listenFields.children_async_data = true;
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            service_message: packServiceMessage('ListenTCPRequest', listenFields),
        };

        const waitMs = timeoutMs > 0 ? timeoutMs + 1000 : DEFAULT_TIMEOUT_MS;
        const reply = await new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pendingListens.delete(socketId);
                reject(new Error('listenTCP timed out'));
            }, waitMs);
            this._pendingListens.set(socketId, {resolve, reject, timer});
            this._sendSignedMessage(request).catch((err) => {
                clearTimeout(timer);
                this._pendingListens.delete(socketId);
                reject(err);
            });
        });

        if (reply && (reply.error || 0) === SUCCESS) {
            const confirmedId = reply.stream_id?.value || socketId;
            if (!reply.stream_id?.value) reply.socket_id = {value: socketId};
            this._streamRoutes.set(confirmedId, nodeId);
        }
        return reply;
    }

    async listenTCP(nodeId, hostPort, options = {}) {
        let reply;
        try { reply = await this.listenTCPEx(nodeId, hostPort, options); } catch { return null; }
        if (!reply || (reply.error || 0) !== SUCCESS || !reply.stream_id?.value) return null;
        return reply.stream_id.value;
    }

    // --- Accept ---

    async acceptTCPEx(listenSocketId, options = {}) {
        const {
            newSocketId, timeoutMs = 0,
            shareChildSocket = false, childUseSocket = false, childAsyncData = false,
        } = options;

        const nodeId = this._streamRoutes.get(listenSocketId);
        if (!nodeId) return null;

        const acceptFields = {listen_stream_id: {value: listenSocketId}};
        if (newSocketId) acceptFields.new_stream_id = {value: normalizeGuid(newSocketId)};
        if (timeoutMs > 0) acceptFields.timeout_ms = timeoutMs;
        if (shareChildSocket) acceptFields.share_child_socket = true;
        if (childUseSocket) acceptFields.child_use_socket = true;
        if (childAsyncData) acceptFields.child_async_data = true;
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            service_message: packServiceMessage('AcceptTCP', acceptFields),
        };

        try { await this._sendSignedMessage(request); } catch { return null; }

        const waitMs = timeoutMs > 0 ? timeoutMs + 1000 : DEFAULT_TIMEOUT_MS;
        let reply;
        try { reply = await this._waitForStreamReply(listenSocketId, waitMs); } catch { return null; }

        if (reply && (reply.error === SUCCESS || reply.error === NEW_CONNECTION) &&
            reply.new_stream_id?.value) {
            this._streamRoutes.set(reply.new_stream_id.value, nodeId);
        }
        return reply || null;
    }

    async acceptTCP(listenSocketId, options = {}) {
        let reply;
        try { reply = await this.acceptTCPEx(listenSocketId, options); } catch { return null; }
        if (!reply || (reply.error !== SUCCESS && reply.error !== NEW_CONNECTION) ||
            !reply.new_stream_id?.value) return null;
        return reply.new_stream_id.value;
    }

    // --- Socket send / recv / close ---

    async streamSend(socketId, data) {
        const nodeId = this._streamRoutes.get(socketId);
        if (!nodeId) {
            trace(`socketSend route-miss socket=${socketId}`);
            return false;
        }

        const dataB64 = Buffer.isBuffer(data) ? data.toString('base64') : Buffer.from(data).toString('base64');
        trace(`socketSend socket=${socketId} node=${nodeId} bytes=${Buffer.from(dataB64, 'base64').length}`);
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            service_message: packServiceMessage('StreamSend', {stream_id: {value: socketId}, data: dataB64}),
        };
        try {
            await this._sendSignedMessage(request);
        } catch (error) {
            trace(`socketSend send-error socket=${socketId} error=${error.message}`);
            return false;
        }

        const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
        while (Date.now() < deadline) {
            const remainingMs = deadline - Date.now();
            let reply;
            try {
                reply = await this._waitForStreamReply(
                    socketId,
                    remainingMs,
                    (candidate) => {
                        const status = candidate?.error || 0;
                        return status !== STREAM_DATA && status !== NEW_CONNECTION && status !== ASYNC_STREAM;
                    }
                );
            } catch (error) {
                trace(`socketSend reply-timeout socket=${socketId} error=${error.message}`);
                return false;
            }

            const status = reply?.error || 0;
            trace(`socketSend reply socket=${socketId} status=${status}`);
            if (status === SUCCESS) {
                return true;
            }

            if (status === STREAM_CLOSED || status === STREAM_ERROR || status === INVALID_FLAGS) {
                return false;
            }
        }

        trace(`socketSend reply-timeout socket=${socketId}`);
        return false;
    }

    async streamRecvEx(socketId, maxBytes = 4096, waitMs = 0) {
        const nodeId = this._streamRoutes.get(socketId);
        if (!nodeId) return null;

        const recvFields = {stream_id: {value: socketId}, max_bytes: maxBytes};
        if (waitMs > 0) recvFields.wait_ms = waitMs;
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            service_message: packServiceMessage('StreamRecv', recvFields),
        };

        try { await this._sendSignedMessage(request); } catch { return null; }

        const replyTimeoutMs = waitMs > 0 ? waitMs + 1000 : DEFAULT_TIMEOUT_MS;
        try { return await this._waitForStreamReply(socketId, replyTimeoutMs); } catch { return null; }
    }

    async streamRecv(socketId, maxBytes = 4096, waitMs = 0) {
        const reply = await this.streamRecvEx(socketId, maxBytes, waitMs);
        if (!reply || (reply.error !== STREAM_DATA && reply.error !== SUCCESS)) return null;
        return reply.data ? Buffer.from(reply.data, 'base64') : Buffer.alloc(0);
    }

    async streamClose(socketId) {
        const nodeId = this._streamRoutes.get(socketId);
        if (!nodeId) {
            trace(`socketClose route-miss socket=${socketId}`);
            return true;
        }

        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            service_message: packServiceMessage('StreamClose', {stream_id: {value: socketId}}),
        };
        try {
            await this._sendSignedMessage(request);
        } catch (error) {
            trace(`socketClose send-error socket=${socketId} error=${error.message}`);
            this._streamRoutes.delete(socketId);
            return true;
        }

        const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
        while (Date.now() < deadline) {
            const remaining = deadline - Date.now();
            let reply;
            try {
                reply = await this._waitForStreamReply(
                    socketId,
                    remaining,
                    (candidate) => {
                        const status = candidate?.error || 0;
                        return status !== STREAM_DATA && status !== NEW_CONNECTION && status !== ASYNC_STREAM;
                    }
                );
            } catch {
                break;
            }
            if (!reply) break;
            if (reply.error === SUCCESS || reply.error === STREAM_CLOSED) {
                this._streamRoutes.delete(socketId);
                return true;
            }
            return false;
        }

        this._streamRoutes.delete(socketId);
        return false;
    }

    // --- Non-blocking dequeue (mirrors C++ tryDequeue* API) ---

    tryDequeueStreamReply() {
        return this._pendingStreamReplies.length > 0 ? this._pendingStreamReplies.shift() : null;
    }

    pendingStreamReplyCount() {
        return this._pendingStreamReplies.length;
    }

    tryDequeueResourceAdvertisement() {
        return this._pendingResourceAdvertisements.length > 0
            ? this._pendingResourceAdvertisements.shift() : null;
    }

    pendingResourceAdvertisementCount() {
        return this._pendingResourceAdvertisements.length;
    }

    tryDequeueResourceQueryReply() {
        return this._pendingResourceQueryReplies.length > 0
            ? this._pendingResourceQueryReplies.shift() : null;
    }

    pendingResourceQueryReplyCount() {
        return this._pendingResourceQueryReplies.length;
    }

    // --- Route registration ---

    registerStreamRoute(socketId, nodeId) {
        this._streamRoutes.set(normalizeGuid(socketId), nodeId);
    }

    // Attach a direct reply handler for a socket, bypassing the queue system.
    // Used by RSPSocket / RSPServer in rsp_net.js for streaming operation.
    // Drains any already-queued replies so no data is lost between connect and attach.
    attachStreamHandler(socketId, handler) {
        this._streamHandlers.set(socketId, handler);
        const queue = this._streamReplyQueues.get(socketId);
        if (queue && queue.length > 0) {
            this._streamReplyQueues.delete(socketId);
            for (const reply of queue) {
                const idx = this._pendingStreamReplies.indexOf(reply);
                if (idx >= 0) this._pendingStreamReplies.splice(idx, 1);
            }
            process.nextTick(() => { for (const reply of queue) handler(reply); });
        }
    }

    detachStreamHandler(socketId) {
        this._streamHandlers.delete(socketId);
    }

    // Fire-and-forget socket send: queues data without waiting for a SUCCESS reply.
    // Use this from streaming code (RSPSocket._write) to avoid blocking on acknowledgement.
    async sendStreamData(socketId, data) {
        const nodeId = this._streamRoutes.get(socketId);
        if (!nodeId) {
            trace(`sendStreamData route-miss socket=${socketId}`);
            return false;
        }
        const dataB64 = Buffer.isBuffer(data) ? data.toString('base64') : Buffer.from(data).toString('base64');
        trace(`sendStreamData socket=${socketId} node=${nodeId} bytes=${Buffer.from(dataB64, 'base64').length}`);
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            stream_send: {stream_id: {value: socketId}, data: dataB64},
        };
        try {
            await this._sendSignedMessage(request);
            trace(`sendStreamData sent socket=${socketId}`);
            return true;
        } catch (error) {
            trace(`sendStreamData error socket=${socketId} error=${error.message}`);
            return false;
        }
    }

    // --- Endorsements ---

    async beginEndorsementRequest(nodeId, endorsementType, endorsementValue = '') {
        const pendingKey = encodeNodeIdForField(nodeId);
        if (this._pendingEndorsements.has(pendingKey)) return null;

        const endorsementValueB64 = Buffer.isBuffer(endorsementValue)
            ? endorsementValue.toString('base64')
            : Buffer.from(endorsementValue).toString('base64');

        const requested = {
            subject: {value: encodeNodeIdForSigner(this.nodeId)},
            endorsement_service: {value: encodeNodeIdForSigner(this.nodeId)},
            endorsement_type: {value: Buffer.from(normalizeGuid(endorsementType), 'hex').toString('base64')},
            endorsement_value: endorsementValueB64,
            valid_until: {milliseconds_since_epoch: Date.now() + 86400000},
        };
        requested.signature = signEndorsement(this._privateKeyPem, this.nodeId, requested);

        let repairedUnknownIdentity = false;
        while (true) {
            let done;
            try {
                done = await this._sendEndorsementRequest(nodeId, pendingKey, requested);
            } catch {
                return null;
            }
            if (!done) return null;

            if (!repairedUnknownIdentity &&
                (done.status || 0) === ENDORSEMENT_UNKNOWN_IDENTITY) {
                repairedUnknownIdentity = true;
                await this._sendIdentityTo(nodeId).catch(() => {});
                continue;
            }

            if ((done.status || 0) === ENDORSEMENT_SUCCESS && done.new_endorsement) {
                this._cacheEndorsement(done.new_endorsement);
            }
            return done;
        }
    }

    async _sendEndorsementRequest(nodeId, pendingKey, requested) {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pendingEndorsements.delete(pendingKey);
                resolve(null);
            }, DEFAULT_TIMEOUT_MS);
            this._pendingEndorsements.set(pendingKey, {resolve, reject, timer});
            const request = {
                destination: {value: encodeNodeIdForField(nodeId)},
                service_message: packServiceMessage('BeginEndorsementRequest', {requested_values: requested}),
            };
            this._sendSignedMessage(request).catch((err) => {
                clearTimeout(timer);
                this._pendingEndorsements.delete(pendingKey);
                reject(err);
            });
        });
    }

    // --- Resource query ---

    async queryResources(nodeId, query = '', maxRecords = 0) {
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            resource_query: {},
        };
        if (query) request.resource_query.query = query;
        if (maxRecords > 0) request.resource_query.max_records = maxRecords;
        try {
            await this._sendSignedMessage(request);
            return true;
        } catch {
            return false;
        }
    }

    async resourceList(nodeId, query = '', maxRecords = 0, timeoutMs = DEFAULT_TIMEOUT_MS) {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pendingResourceList = null;
                resolve(null);
            }, timeoutMs);
            this._pendingResourceList = {resolve, reject, timer};
            this.queryResources(nodeId, query, maxRecords).catch((err) => {
                clearTimeout(timer);
                this._pendingResourceList = null;
                reject(err);
            });
        });
    }

    // --- Name service ---

    _handleNameReply(msg) {
        if (this._pendingNameReply) {
            const {resolve, timer} = this._pendingNameReply;
            this._pendingNameReply = null;
            clearTimeout(timer);
            resolve(unpackServiceMessage(msg));
        }
    }

    async _sendNameRequest(nodeId, typeName, fields, timeoutMs = DEFAULT_TIMEOUT_MS) {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pendingNameReply = null;
                resolve(null);
            }, timeoutMs);
            this._pendingNameReply = {resolve, reject, timer};
            this._sendSignedMessage({
                destination: {value: encodeNodeIdForField(nodeId)},
                service_message: packServiceMessage(typeName, fields),
            }).catch((err) => {
                clearTimeout(timer);
                this._pendingNameReply = null;
                reject(err);
            });
        });
    }

    async nameCreate(nodeId, name, owner, type, value, timeoutMs = DEFAULT_TIMEOUT_MS) {
        return this._sendNameRequest(nodeId, 'NameCreateRequest', {
            record: {
                name,
                owner: {value: encodeNodeIdForField(owner)},
                type: {value: encodeNodeIdForField(type)},
                value: {value: encodeNodeIdForField(value)},
            },
        }, timeoutMs);
    }

    async nameRead(nodeId, name, owner = null, type = null, timeoutMs = DEFAULT_TIMEOUT_MS) {
        const fields = {name};
        if (owner) fields.owner = {value: encodeNodeIdForField(owner)};
        if (type) fields.type = {value: encodeNodeIdForField(type)};
        return this._sendNameRequest(nodeId, 'NameReadRequest', fields, timeoutMs);
    }

    async nameUpdate(nodeId, name, owner, type, newValue, timeoutMs = DEFAULT_TIMEOUT_MS) {
        return this._sendNameRequest(nodeId, 'NameUpdateRequest', {
            name,
            owner: {value: encodeNodeIdForField(owner)},
            type: {value: encodeNodeIdForField(type)},
            new_value: {value: encodeNodeIdForField(newValue)},
        }, timeoutMs);
    }

    async nameDelete(nodeId, name, owner, type, timeoutMs = DEFAULT_TIMEOUT_MS) {
        return this._sendNameRequest(nodeId, 'NameDeleteRequest', {
            name,
            owner: {value: encodeNodeIdForField(owner)},
            type: {value: encodeNodeIdForField(type)},
        }, timeoutMs);
    }

    async nameQuery(nodeId, namePrefix = '', owner = null, type = null, maxRecords = 0, timeoutMs = DEFAULT_TIMEOUT_MS) {
        const fields = {};
        if (namePrefix) fields.name_prefix = namePrefix;
        if (owner) fields.owner = {value: encodeNodeIdForField(owner)};
        if (type) fields.type = {value: encodeNodeIdForField(type)};
        if (maxRecords > 0) fields.max_records = maxRecords;
        return this._sendNameRequest(nodeId, 'NameQueryRequest', fields, timeoutMs);
    }
}

module.exports = {
    RSPClient,
    decodeNodeIdField,
    encodeNodeIdForField,
    encodeNodeIdForSigner,
    nodeIdFromPublicKeyPem,
};