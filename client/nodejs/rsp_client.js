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
    SOCKET_CLOSED,
    SOCKET_DATA,
    SOCKET_ERROR,
    NEW_CONNECTION,
    ASYNC_SOCKET,
    INVALID_FLAGS,
    SOCKET_IN_USE,
} = messages.SOCKET_STATUS;

const {
    ENDORSEMENT_SUCCESS,
    ENDORSEMENT_UNKNOWN_IDENTITY,
} = messages.ENSDORSMENT_STATUS;

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
    return bytes.toString('hex');
}

function decodeNodeIdField(hexValue) {
    const bytes = Buffer.from(hexValue, 'hex');
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
    return bytes.toString('hex');
}

function decodeSignerNodeId(hexValue) {
    return guidFromBytes(Buffer.from(hexValue, 'hex'));
}

function randomUuidHex() {
    return crypto.randomBytes(16).toString('hex');
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
        signature: signer.sign(privateKeyPem).toString('hex'),
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
    return verifier.verify(publicKeyPem, Buffer.from(signatureBlock.signature, 'hex'));
}

function signEndorsement(privateKeyPem, localNodeId, endorsement) {
    // Hash the endorsement without its signature field, then sign the digest.
    const unsignedEndorsement = Object.assign({}, endorsement);
    delete unsignedEndorsement.signature;
    const digest = messages.hashEndorsement(unsignedEndorsement);
    const signer = crypto.createSign('sha256');
    signer.update(digest);
    signer.end();
    return signer.sign(privateKeyPem).toString('hex');
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
    constructor(keyPair) {
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

        this.peerNodeId = null;
        this.peerPublicKeyPem = null;

        this._pingSequence = 1;
        this._pendingPings = new Map();        // nonce -> {sequence, resolve, reject, timer}

        this._pendingConnects = new Map();     // socketId -> {resolve, reject, timer}
        this._pendingListens = new Map();      // socketId -> {resolve, reject, timer}
        this._socketRoutes = new Map();        // socketId -> nodeId
        this._socketReplyQueues = new Map();   // socketId -> SocketReply[]
        this._awaitedSocketReplies = new Map();// socketId -> {resolve, reject, timer}
        this._pendingSocketReplies = [];       // global async queue

        this._pendingEndorsements = new Map(); // nodeIdHex -> {resolve, reject, timer}
        this._endorsementCache = new Map();    // `${nodeId}:${typeHex}` -> endorsement

        this._pendingResourceAdvertisements = [];
        this._identityCache = new Map();       // nodeId -> publicKeyPem
    }

    // --- Connection lifecycle ---

    async connect(transportSpec) {
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
        if (!this._socket) return;

        this._stopping = true;
        const socket = this._socket;
        this._socket = null;
        this._reader = null;
        this.peerNodeId = null;
        this.peerPublicKeyPem = null;

        const closedError = new Error('client closed');
        for (const [, p] of this._pendingPings) { clearTimeout(p.timer); p.reject(closedError); }
        this._pendingPings.clear();
        for (const [, p] of this._pendingConnects) { clearTimeout(p.timer); p.reject(closedError); }
        this._pendingConnects.clear();
        for (const [, p] of this._pendingListens) { clearTimeout(p.timer); p.reject(closedError); }
        this._pendingListens.clear();
        for (const [, p] of this._awaitedSocketReplies) { clearTimeout(p.timer); p.reject(closedError); }
        this._awaitedSocketReplies.clear();
        for (const [, p] of this._pendingEndorsements) { clearTimeout(p.timer); p.reject(closedError); }
        this._pendingEndorsements.clear();

        socket.end();
        await once(socket, 'close').catch(() => {});
        if (this._receiveLoopPromise) {
            await this._receiveLoopPromise.catch(() => {});
        }
    }

    // --- Identity exchange ---

    async _performInitialIdentityExchange() {
        const localChallengeNonce = randomUuidHex();
        await this._sendRawMessage({challenge_request: {nonce: {value: localChallengeNonce}}});

        let peerChallengeReceived = false;
        let peerIdentityReceived = false;

        while (!peerChallengeReceived || !peerIdentityReceived) {
            const message = await this._receiveRawMessage();

            if (hasField(message, 'challenge_request')) {
                const nonceHex = message.challenge_request?.nonce?.value;
                if (message.destination || message.signature || peerChallengeReceived ||
                    !nonceHex || nonceHex.length !== 32) {
                    throw new Error('received an invalid challenge request during authentication');
                }
                const identityMessage = {
                    identity: {
                        nonce: {value: nonceHex},
                        public_key: {
                            algorithm: P256_ALGORITHM,
                            public_key: Buffer.from(this._publicKeyPem, 'utf8').toString('hex'),
                        },
                    },
                };
                identityMessage.signature = signRSPMessage(this._privateKeyPem, this.nodeId, identityMessage);
                await this._sendRawMessage(identityMessage);
                peerChallengeReceived = true;
                continue;
            }

            if (hasField(message, 'identity')) {
                const peerPublicKeyPem = Buffer.from(message.identity.public_key.public_key, 'hex').toString('utf8');
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
            identity: {
                public_key: {
                    algorithm: P256_ALGORITHM,
                    public_key: Buffer.from(this._publicKeyPem, 'utf8').toString('hex'),
                },
            },
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
            } catch {
                if (!this._stopping) throw new Error('receive loop ended unexpectedly');
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

        if (hasField(msg, 'ping_reply')) {
            this._handlePingReply(msg);
        } else if (hasField(msg, 'socket_reply')) {
            this._handleSocketReply(msg, msg.socket_reply);
        } else if (hasField(msg, 'endorsement_done')) {
            this._handleEndorsementDone(msg);
        } else if (hasField(msg, 'endorsement_needed')) {
            this.emit('endorsement_needed', msg.endorsement_needed);
        } else if (hasField(msg, 'resource_advertisement')) {
            this._pendingResourceAdvertisements.push(msg.resource_advertisement);
            this.emit('resource_advertisement', msg.resource_advertisement);
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
        pending.resolve(msg);
    }

    _handleSocketReply(msg, socketReply) {
        const socketIdHex = socketReply.socket_id?.value;

        if (socketIdHex) {
            const connectPending = this._pendingConnects.get(socketIdHex);
            if (connectPending) {
                const status = socketReply.error || 0;
                if (status === SUCCESS || status === CONNECT_REFUSED || status === CONNECT_TIMEOUT ||
                    status === SOCKET_ERROR || status === SOCKET_IN_USE || status === INVALID_FLAGS) {
                    clearTimeout(connectPending.timer);
                    this._pendingConnects.delete(socketIdHex);
                    connectPending.resolve(socketReply);
                    return;
                }
            }

            const listenPending = this._pendingListens.get(socketIdHex);
            if (listenPending) {
                const status = socketReply.error || 0;
                if (status === SUCCESS || status === SOCKET_ERROR ||
                    status === SOCKET_IN_USE || status === INVALID_FLAGS) {
                    clearTimeout(listenPending.timer);
                    this._pendingListens.delete(socketIdHex);
                    listenPending.resolve(socketReply);
                    return;
                }
            }

            if (socketReply.new_socket_id?.value) {
                const sourceNodeId = this._decodeSourceNodeId(msg);
                if (sourceNodeId) {
                    this._socketRoutes.set(socketReply.new_socket_id.value, sourceNodeId);
                }
            }

            const queue = this._socketReplyQueues.get(socketIdHex) || [];
            queue.push(socketReply);
            this._socketReplyQueues.set(socketIdHex, queue);

            const awaited = this._awaitedSocketReplies.get(socketIdHex);
            if (awaited) {
                this._awaitedSocketReplies.delete(socketIdHex);
                queue.shift();
                if (queue.length === 0) this._socketReplyQueues.delete(socketIdHex);
                clearTimeout(awaited.timer);
                awaited.resolve(socketReply);
                return;
            }
        }

        this._pendingSocketReplies.push(socketReply);
        this.emit('socket_reply', socketIdHex, socketReply);
    }

    _handleEndorsementDone(msg) {
        const sourceNodeId = this._decodeSourceNodeId(msg);
        if (!sourceNodeId) return;
        const pendingKey = encodeNodeIdForField(sourceNodeId);
        const pending = this._pendingEndorsements.get(pendingKey);
        if (!pending) return;
        clearTimeout(pending.timer);
        this._pendingEndorsements.delete(pendingKey);
        pending.resolve(msg.endorsement_done);
    }

    _decodeSourceNodeId(msg) {
        if (!msg.source?.value) return null;
        try {
            return decodeNodeIdField(msg.source.value);
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

    // --- Message send/receive primitives ---

    async _sendRawMessage(message) {
        if (!this._socket) throw new Error('client is not connected');
        const payload = Buffer.from(JSON.stringify(message), 'utf8');
        const header = Buffer.alloc(8);
        header.writeUInt32BE(JSON_FRAME_MAGIC, 0);
        header.writeUInt32BE(payload.length, 4);
        await new Promise((resolve, reject) => {
            this._socket.write(Buffer.concat([header, payload]), (err) => err ? reject(err) : resolve());
        });
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
        return JSON.parse(payload.toString('utf8'));
    }

    async _sendSignedMessage(message) {
        const signed = Object.assign({}, message, {
            signature: signRSPMessage(this._privateKeyPem, this.nodeId, message),
        });
        await this._sendRawMessage(signed);
    }

    // --- Socket reply waiting ---

    _waitForSocketReply(socketId, timeoutMs = DEFAULT_TIMEOUT_MS) {
        const queue = this._socketReplyQueues.get(socketId);
        if (queue && queue.length > 0) {
            const reply = queue.shift();
            if (queue.length === 0) this._socketReplyQueues.delete(socketId);
            const idx = this._pendingSocketReplies.indexOf(reply);
            if (idx >= 0) this._pendingSocketReplies.splice(idx, 1);
            return Promise.resolve(reply);
        }
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._awaitedSocketReplies.delete(socketId);
                reject(new Error(`socket reply timed out for socket ${socketId}`));
            }, timeoutMs);
            this._awaitedSocketReplies.set(socketId, {resolve, reject, timer});
        });
    }

    // --- Ping ---

    async ping(nodeId, timeoutMs = DEFAULT_TIMEOUT_MS) {
        const pingNonce = randomUuidHex();
        const sequence = this._pingSequence++;
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            nonce: {value: randomUuidHex()},
            ping_request: {
                nonce: {value: pingNonce},
                sequence,
                time_sent: {milliseconds_since_epoch: Date.now()},
            },
        };
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pendingPings.delete(pingNonce);
                reject(new Error('ping timed out'));
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

        const socketId = randomUuidHex();
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            connect_tcp_request: {
                host_port: hostPort,
                socket_number: {value: socketId},
            },
        };
        if (useSocket) request.connect_tcp_request.use_socket = true;
        if (timeoutMs > 0) request.connect_tcp_request.timeout_ms = timeoutMs;
        if (retries > 0) request.connect_tcp_request.retries = retries;
        if (retryMs > 0) request.connect_tcp_request.retry_ms = retryMs;
        if (asyncData) request.connect_tcp_request.async_data = true;
        if (shareSocket) request.connect_tcp_request.share_socket = true;

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
            const confirmedId = reply.socket_id?.value || socketId;
            if (!reply.socket_id?.value) reply.socket_id = {value: socketId};
            this._socketRoutes.set(confirmedId, nodeId);
        }
        return reply;
    }

    async connectTCP(nodeId, hostPort, options = {}) {
        let reply;
        try { reply = await this.connectTCPEx(nodeId, hostPort, options); } catch { return null; }
        if (!reply || (reply.error || 0) !== SUCCESS || !reply.socket_id?.value) return null;
        return reply.socket_id.value;
    }

    // --- Listen ---

    async listenTCPEx(nodeId, hostPort, options = {}) {
        const {
            timeoutMs = 0, asyncAccept = false, shareListeningSocket = false,
            shareChildSockets = false, childrenUseSocket = false, childrenAsyncData = false,
        } = options;

        const socketId = randomUuidHex();
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            listen_tcp_request: {
                host_port: hostPort,
                socket_number: {value: socketId},
            },
        };
        if (timeoutMs > 0) request.listen_tcp_request.timeout_ms = timeoutMs;
        if (asyncAccept) request.listen_tcp_request.async_accept = true;
        if (shareListeningSocket) request.listen_tcp_request.share_listening_socket = true;
        if (shareChildSockets) request.listen_tcp_request.share_child_sockets = true;
        if (childrenUseSocket) request.listen_tcp_request.children_use_socket = true;
        if (childrenAsyncData) request.listen_tcp_request.children_async_data = true;

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
            const confirmedId = reply.socket_id?.value || socketId;
            if (!reply.socket_id?.value) reply.socket_id = {value: socketId};
            this._socketRoutes.set(confirmedId, nodeId);
        }
        return reply;
    }

    async listenTCP(nodeId, hostPort, options = {}) {
        let reply;
        try { reply = await this.listenTCPEx(nodeId, hostPort, options); } catch { return null; }
        if (!reply || (reply.error || 0) !== SUCCESS || !reply.socket_id?.value) return null;
        return reply.socket_id.value;
    }

    // --- Accept ---

    async acceptTCPEx(listenSocketId, options = {}) {
        const {
            newSocketId, timeoutMs = 0,
            shareChildSocket = false, childUseSocket = false, childAsyncData = false,
        } = options;

        const nodeId = this._socketRoutes.get(listenSocketId);
        if (!nodeId) return null;

        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            accept_tcp: {listen_socket_number: {value: listenSocketId}},
        };
        if (newSocketId) request.accept_tcp.new_socket_number = {value: normalizeGuid(newSocketId)};
        if (timeoutMs > 0) request.accept_tcp.timeout_ms = timeoutMs;
        if (shareChildSocket) request.accept_tcp.share_child_socket = true;
        if (childUseSocket) request.accept_tcp.child_use_socket = true;
        if (childAsyncData) request.accept_tcp.child_async_data = true;

        try { await this._sendSignedMessage(request); } catch { return null; }

        const waitMs = timeoutMs > 0 ? timeoutMs + 1000 : DEFAULT_TIMEOUT_MS;
        let reply;
        try { reply = await this._waitForSocketReply(listenSocketId, waitMs); } catch { return null; }

        if (reply && (reply.error === SUCCESS || reply.error === NEW_CONNECTION) &&
            reply.new_socket_id?.value) {
            this._socketRoutes.set(reply.new_socket_id.value, nodeId);
        }
        return reply || null;
    }

    async acceptTCP(listenSocketId, options = {}) {
        let reply;
        try { reply = await this.acceptTCPEx(listenSocketId, options); } catch { return null; }
        if (!reply || (reply.error !== SUCCESS && reply.error !== NEW_CONNECTION) ||
            !reply.new_socket_id?.value) return null;
        return reply.new_socket_id.value;
    }

    // --- Socket send / recv / close ---

    async socketSend(socketId, data) {
        const nodeId = this._socketRoutes.get(socketId);
        if (!nodeId) return false;

        const dataHex = Buffer.isBuffer(data) ? data.toString('hex') : Buffer.from(data).toString('hex');
        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            socket_send: {socket_number: {value: socketId}, data: dataHex},
        };
        try { await this._sendSignedMessage(request); } catch { return false; }

        let reply;
        try { reply = await this._waitForSocketReply(socketId); } catch { return false; }
        return reply && (reply.error || 0) === SUCCESS;
    }

    async socketRecvEx(socketId, maxBytes = 4096, waitMs = 0) {
        const nodeId = this._socketRoutes.get(socketId);
        if (!nodeId) return null;

        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            socket_recv: {socket_number: {value: socketId}, max_bytes: maxBytes},
        };
        if (waitMs > 0) request.socket_recv.wait_ms = waitMs;

        try { await this._sendSignedMessage(request); } catch { return null; }

        const replyTimeoutMs = waitMs > 0 ? waitMs + 1000 : DEFAULT_TIMEOUT_MS;
        try { return await this._waitForSocketReply(socketId, replyTimeoutMs); } catch { return null; }
    }

    async socketRecv(socketId, maxBytes = 4096, waitMs = 0) {
        const reply = await this.socketRecvEx(socketId, maxBytes, waitMs);
        if (!reply || (reply.error !== SOCKET_DATA && reply.error !== SUCCESS)) return null;
        return reply.data ? Buffer.from(reply.data, 'hex') : Buffer.alloc(0);
    }

    async socketClose(socketId) {
        const nodeId = this._socketRoutes.get(socketId);
        if (!nodeId) return false;

        const request = {
            destination: {value: encodeNodeIdForField(nodeId)},
            socket_close: {socket_number: {value: socketId}},
        };
        try {
            await this._sendSignedMessage(request);
        } catch {
            this._socketRoutes.delete(socketId);
            return true;
        }

        const deadline = Date.now() + DEFAULT_TIMEOUT_MS;
        while (Date.now() < deadline) {
            const remaining = deadline - Date.now();
            let reply;
            try { reply = await this._waitForSocketReply(socketId, remaining); } catch { break; }
            if (!reply) break;
            if (reply.error === SUCCESS || reply.error === SOCKET_CLOSED) {
                this._socketRoutes.delete(socketId);
                return true;
            }
            if (reply.error !== SOCKET_DATA && reply.error !== ASYNC_SOCKET && reply.error !== NEW_CONNECTION) {
                return false;
            }
        }

        this._socketRoutes.delete(socketId);
        return true;
    }

    // --- Non-blocking dequeue (mirrors C++ tryDequeue* API) ---

    tryDequeueSocketReply() {
        return this._pendingSocketReplies.length > 0 ? this._pendingSocketReplies.shift() : null;
    }

    pendingSocketReplyCount() {
        return this._pendingSocketReplies.length;
    }

    tryDequeueResourceAdvertisement() {
        return this._pendingResourceAdvertisements.length > 0
            ? this._pendingResourceAdvertisements.shift() : null;
    }

    pendingResourceAdvertisementCount() {
        return this._pendingResourceAdvertisements.length;
    }

    // --- Route registration ---

    registerSocketRoute(socketId, nodeId) {
        this._socketRoutes.set(normalizeGuid(socketId), nodeId);
    }

    // --- Endorsements ---

    async beginEndorsementRequest(nodeId, endorsementType, endorsementValue = '') {
        const pendingKey = encodeNodeIdForField(nodeId);
        if (this._pendingEndorsements.has(pendingKey)) return null;

        const endorsementValueHex = Buffer.isBuffer(endorsementValue)
            ? endorsementValue.toString('hex')
            : Buffer.from(endorsementValue).toString('hex');

        const requested = {
            subject: {value: encodeNodeIdForSigner(this.nodeId)},
            endorsement_service: {value: encodeNodeIdForSigner(nodeId)},
            endorsement_type: {value: normalizeGuid(endorsementType)},
            endorsement_value: endorsementValueHex,
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

            if (done.status === ENDORSEMENT_SUCCESS && done.new_endorsement) {
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
                begin_endorsement_request: {requested_values: requested},
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
}

// Backward-compatible alias used by ping.js and other existing callers.
const RSPJsonClient = RSPClient;

module.exports = {
    RSPClient,
    RSPJsonClient,
    decodeNodeIdField,
    nodeIdFromPublicKeyPem,
};