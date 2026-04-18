'use strict';

// JSON encoding plugin for the RSP full client.
//
// Implements the Encoding interface:
//   get handshakeToken()          – string sent during ASCII handshake negotiation
//   readFrame(reader)  -> Promise<object>  – read one RSP message from the transport reader
//   writeFrame(send, message)     – write one RSP message via the transport send function
//
// Wire format: [magic:4BE][length:4BE][JSON-payload:length]
// Magic: 0x5253504A  ('RSPJ')

const FRAME_MAGIC = 0x5253504a;
const MAX_FRAME_LENGTH = 16 * 1024 * 1024;

class JsonEncoding {
    get handshakeToken() {
        return 'encoding:json';
    }

    encode(message) {
        return Buffer.from(JSON.stringify(message), 'utf8');
    }

    decode(buffer) {
        return JSON.parse(buffer.toString('utf8'));
    }

    async readFrame(reader) {
        const header = await reader.readExact(8);
        const magic = header.readUInt32BE(0);
        if (magic !== FRAME_MAGIC) {
            throw new Error(`json encoding: bad frame magic 0x${magic.toString(16).padStart(8, '0')}`);
        }
        const length = header.readUInt32BE(4);
        if (length > MAX_FRAME_LENGTH) {
            throw new Error(`json encoding: frame too large (${length} bytes)`);
        }
        const payload = await reader.readExact(length);
        return this.decode(payload);
    }

    async writeFrame(send, message) {
        const payload = this.encode(message);
        if (payload.length > MAX_FRAME_LENGTH) {
            throw new Error(`json encoding: message too large to send (${payload.length} bytes)`);
        }
        const header = Buffer.alloc(8);
        header.writeUInt32BE(FRAME_MAGIC, 0);
        header.writeUInt32BE(payload.length, 4);
        await send(Buffer.concat([header, payload]));
    }
}

module.exports = {JsonEncoding};
