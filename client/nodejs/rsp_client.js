'use strict';

const crypto = require('crypto');
const net = require('net');
const os = require('os');
const {EventEmitter, once} = require('events');

const JSON_FRAME_MAGIC = 0x5253504a;
const HANDSHAKE_TERMINATOR = Buffer.from('\r\n\r\n', 'ascii');
const P256_ALGORITHM = 100;

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

function hexBytes(hexValue) {
    return Buffer.from(hexValue || '', 'hex');
}

class MessageHasher {
    constructor() {
        this.hash = crypto.createHash('sha256');
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
        buffer.writeBigUInt64BE(BigInt(value), 0);
        this.feed(buffer);
    }

    feedBool(value) {
        this.feedUint8(value ? 1 : 0);
    }

    feedBytes(bytes) {
        const buffer = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes || '', 'utf8');
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

function hashNodeId(hasher, message) {
    hasher.tag(1);
    hasher.feedBytes(hexBytes(message.value));
}

function hashUuid(hasher, message) {
    hasher.tag(1);
    hasher.feedBytes(hexBytes(message.value));
}

function hashDateTime(hasher, message) {
    hasher.tag(1);
    hasher.feedUint64(BigInt(message.milliseconds_since_epoch || 0));
}

function hashPublicKey(hasher, message) {
    hasher.tag(1);
    hasher.feedUint32(message.algorithm >>> 0);
    hasher.tag(2);
    hasher.feedBytes(hexBytes(message.public_key));
}

function hashEndorsement(hasher, message) {
    if (hasField(message, 'subject')) {
        hasher.tag(1);
        hashNodeId(hasher, message.subject);
    }
    if (hasField(message, 'endorsement_service')) {
        hasher.tag(2);
        hashNodeId(hasher, message.endorsement_service);
    }
    if (hasField(message, 'endorsement_type')) {
        hasher.tag(3);
        hashUuid(hasher, message.endorsement_type);
    }
    hasher.tag(4);
    hasher.feedBytes(hexBytes(message.endorsement_value));
    if (hasField(message, 'valid_until')) {
        hasher.tag(5);
        hashDateTime(hasher, message.valid_until);
    }
    hasher.tag(99);
    hasher.feedBytes(hexBytes(message.signature));
}

function hashChallengeRequest(hasher, message) {
    if (hasField(message, 'nonce')) {
        hasher.tag(1);
        hashUuid(hasher, message.nonce);
    }
}

function hashIdentity(hasher, message) {
    if (hasField(message, 'nonce')) {
        hasher.tag(1);
        hashUuid(hasher, message.nonce);
    }
    if (hasField(message, 'public_key')) {
        hasher.tag(2);
        hashPublicKey(hasher, message.public_key);
    }
}

function hashPingRequest(hasher, message) {
    if (hasField(message, 'nonce')) {
        hasher.tag(1);
        hashUuid(hasher, message.nonce);
    }
    hasher.tag(2);
    hasher.feedUint32(message.sequence >>> 0);
    if (hasField(message, 'time_sent')) {
        hasher.tag(3);
        hashDateTime(hasher, message.time_sent);
    }
}

function hashPingReply(hasher, message) {
    if (hasField(message, 'nonce')) {
        hasher.tag(1);
        hashUuid(hasher, message.nonce);
    }
    hasher.tag(2);
    hasher.feedUint32(message.sequence >>> 0);
    if (hasField(message, 'time_sent')) {
        hasher.tag(3);
        hashDateTime(hasher, message.time_sent);
    }
    if (hasField(message, 'time_replied')) {
        hasher.tag(4);
        hashDateTime(hasher, message.time_replied);
    }
}

function hashError(hasher, message) {
    hasher.tag(1);
    hasher.feedUint32(message.error_code >>> 0);
    hasher.tag(2);
    hasher.feedBytes(Buffer.from(message.message || '', 'utf8'));
}

function hashRSPMessage(message) {
    const hasher = new MessageHasher();

    if (hasField(message, 'destination')) {
        hasher.tag(1);
        hashNodeId(hasher, message.destination);
    }
    if (hasField(message, 'source')) {
        hasher.tag(2);
        hashNodeId(hasher, message.source);
    }

    if (hasField(message, 'challenge_request')) {
        hasher.tag(3);
        hashChallengeRequest(hasher, message.challenge_request);
    } else if (hasField(message, 'identity')) {
        hasher.tag(4);
        hashIdentity(hasher, message.identity);
    } else if (hasField(message, 'error')) {
        hasher.tag(6);
        hashError(hasher, message.error);
    } else if (hasField(message, 'ping_request')) {
        hasher.tag(7);
        hashPingRequest(hasher, message.ping_request);
    } else if (hasField(message, 'ping_reply')) {
        hasher.tag(8);
        hashPingReply(hasher, message.ping_reply);
    } else {
        throw new Error('unsupported RSP message type for the Node.js client');
    }

    if (hasField(message, 'nonce')) {
        hasher.tag(22);
        hashUuid(hasher, message.nonce);
    }

    const endorsements = Array.isArray(message.endorsements) ? message.endorsements : [];
    hasher.tag(100);
    hasher.feedUint32(endorsements.length >>> 0);
    for (const endorsement of endorsements) {
        hashEndorsement(hasher, endorsement);
    }

    return hasher.finalize();
}

function signMessage(privateKeyPem, localNodeId, message) {
    const digest = hashRSPMessage(message);
    const signer = crypto.createSign('sha256');
    signer.update(digest);
    signer.end();

    return {
        signer: {value: encodeNodeIdForSigner(localNodeId)},
        algorithm: P256_ALGORITHM,
        signature: signer.sign(privateKeyPem).toString('hex'),
    };
}

function verifyMessageSignature(publicKeyPem, message, signatureBlock) {
    if (!signatureBlock || signatureBlock.algorithm !== P256_ALGORITHM) {
        return false;
    }

    if (decodeSignerNodeId(signatureBlock.signer.value) !== nodeIdFromPublicKeyPem(publicKeyPem)) {
        return false;
    }

    const digest = hashRSPMessage(message);
    const verifier = crypto.createVerify('sha256');
    verifier.update(digest);
    verifier.end();
    return verifier.verify(publicKeyPem, Buffer.from(signatureBlock.signature, 'hex'));
}

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
        socket.on('end', () => {
            this.ended = true;
            this.events.emit('update');
        });
        socket.on('close', () => {
            this.ended = true;
            this.events.emit('update');
        });
        socket.on('error', (error) => {
            this.error = error;
            this.events.emit('update');
        });
    }

    async waitForData() {
        if (this.error) {
            throw this.error;
        }
        if (this.ended) {
            throw new Error('socket closed while waiting for data');
        }
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

class RSPJsonClient {
    constructor(keyPair) {
        const generated = keyPair || crypto.generateKeyPairSync('ec', {
            namedCurve: 'prime256v1',
            privateKeyEncoding: {type: 'pkcs8', format: 'pem'},
            publicKeyEncoding: {type: 'spki', format: 'pem'},
        });

        this.privateKeyPem = generated.privateKey;
        this.publicKeyPem = generated.publicKey;
        this.nodeId = nodeIdFromPublicKeyPem(this.publicKeyPem);
        this.peerNodeId = null;
        this.peerPublicKeyPem = null;
        this.sequence = 1;
        this.socket = null;
        this.reader = null;
    }

    async connect(transportSpec) {
        const endpoint = parseTransportSpec(transportSpec);
        const socket = net.createConnection(endpoint);
        socket.setNoDelay(true);
        await once(socket, 'connect');

        this.socket = socket;
        this.reader = new BufferedSocketReader(socket);

        await this.reader.readUntil(HANDSHAKE_TERMINATOR);
        socket.write('encoding:json\r\n\r\n', 'ascii');

        const result = (await this.reader.readUntil(HANDSHAKE_TERMINATOR)).toString('ascii');
        if (!result.startsWith('1success: encoding:json')) {
            throw new Error(`ASCII handshake failed: ${result.trim()}`);
        }

        await this.performInitialIdentityExchange();
    }

    async close() {
        if (!this.socket) {
            return;
        }

        const socket = this.socket;
        this.socket = null;
        this.reader = null;
        this.peerNodeId = null;
        this.peerPublicKeyPem = null;
        socket.end();
        await once(socket, 'close').catch(() => {});
    }

    async performInitialIdentityExchange() {
        const localChallengeNonce = randomUuidHex();
        await this.sendUnsigned({challenge_request: {nonce: {value: localChallengeNonce}}});

        let peerChallengeReceived = false;
        let peerIdentityReceived = false;

        while (!peerChallengeReceived || !peerIdentityReceived) {
            const message = await this.receiveMessage();

            if (hasField(message, 'challenge_request')) {
                const nonceHex = message.challenge_request?.nonce?.value;
                if (message.destination || message.signature || peerChallengeReceived || !nonceHex || nonceHex.length !== 32) {
                    throw new Error('received an invalid challenge request during authentication');
                }

                const identityMessage = {
                    identity: {
                        nonce: {value: nonceHex},
                        public_key: {
                            algorithm: P256_ALGORITHM,
                            public_key: Buffer.from(this.publicKeyPem, 'utf8').toString('hex'),
                        },
                    },
                };
                identityMessage.signature = signMessage(this.privateKeyPem, this.nodeId, identityMessage);
                await this.sendRawMessage(identityMessage);
                peerChallengeReceived = true;
                continue;
            }

            if (hasField(message, 'identity')) {
                const peerPublicKeyPem = Buffer.from(message.identity.public_key.public_key, 'hex').toString('utf8');
                if (message.identity?.nonce?.value !== localChallengeNonce ||
                    !verifyMessageSignature(peerPublicKeyPem, message, message.signature)) {
                    throw new Error('received an invalid identity response during authentication');
                }

                this.peerPublicKeyPem = peerPublicKeyPem;
                this.peerNodeId = nodeIdFromPublicKeyPem(peerPublicKeyPem);
                peerIdentityReceived = true;
                continue;
            }

            throw new Error('received an unexpected message during authentication');
        }
    }

    async ping(destinationNodeId, timeoutMilliseconds = 5000) {
        const pingNonce = randomUuidHex();
        const sequence = this.sequence++;
        const request = {
            destination: {value: encodeNodeIdForField(destinationNodeId)},
            nonce: {value: randomUuidHex()},
            ping_request: {
                nonce: {value: pingNonce},
                sequence,
                time_sent: {milliseconds_since_epoch: Date.now()},
            },
        };

        request.signature = signMessage(this.privateKeyPem, this.nodeId, request);
        await this.sendRawMessage(request);

        const deadline = Date.now() + timeoutMilliseconds;
        while (true) {
            const remaining = deadline - Date.now();
            if (remaining <= 0) {
                throw new Error('ping timed out');
            }

            const message = await Promise.race([
                this.receiveMessage(),
                new Promise((_, reject) => setTimeout(() => reject(new Error('ping timed out')), remaining)),
            ]);

            if (hasField(message, 'error')) {
                throw new Error(`received error reply: ${message.error.message || 'unknown error'}`);
            }
            if (!hasField(message, 'ping_reply')) {
                continue;
            }
            if (!hasField(message, 'destination') || decodeNodeIdField(message.destination.value) !== this.nodeId) {
                continue;
            }
            if (message.ping_reply.nonce?.value === pingNonce && message.ping_reply.sequence === sequence) {
                return message;
            }
        }
    }

    async sendUnsigned(message) {
        await this.sendRawMessage(message);
    }

    async sendRawMessage(message) {
        if (!this.socket) {
            throw new Error('client is not connected');
        }

        const payload = Buffer.from(JSON.stringify(message), 'utf8');
        const header = Buffer.alloc(8);
        header.writeUInt32BE(JSON_FRAME_MAGIC, 0);
        header.writeUInt32BE(payload.length, 4);
        const frame = Buffer.concat([header, payload]);

        await new Promise((resolve, reject) => {
            this.socket.write(frame, (error) => {
                if (error) {
                    reject(error);
                } else {
                    resolve();
                }
            });
        });
    }

    async receiveMessage() {
        if (!this.reader) {
            throw new Error('client is not connected');
        }

        const header = await this.reader.readExact(8);
        const magic = header.readUInt32BE(0);
        if (magic !== JSON_FRAME_MAGIC) {
            throw new Error(`unexpected JSON frame magic: 0x${magic.toString(16)}`);
        }
        const payloadLength = header.readUInt32BE(4);
        const payload = await this.reader.readExact(payloadLength);
        return JSON.parse(payload.toString('utf8'));
    }
}

module.exports = {
    RSPJsonClient,
    decodeNodeIdField,
    nodeIdFromPublicKeyPem,
};