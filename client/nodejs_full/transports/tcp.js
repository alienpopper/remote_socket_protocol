'use strict';

// TCP transport plugin for the RSP full client.
//
// Implements the Transport interface:
//   connect(transportSpec) -> { reader, send, socket, close }
//
// `reader`  – BufferedSocketReader for the encoding plugin to read frames.
// `send`    – async (Buffer) -> void, writes raw bytes to the socket.
// `socket`  – underlying net.Socket, handed to RSPClient for teardown compat.
// `close`   – async () -> void, gracefully closes the connection.

const net = require('net');
const {EventEmitter, once} = require('events');

// BufferedSocketReader accumulates inbound bytes from a net.Socket and
// provides readUntil/readExact helpers used by encoding plugins.
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
        socket.on('error', (err) => { this.error = err; this.events.emit('update'); });
    }

    async _waitForData() {
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
            await this._waitForData();
        }
    }

    async readExact(length) {
        while (this.buffer.length < length) {
            await this._waitForData();
        }
        const chunk = this.buffer.subarray(0, length);
        this.buffer = this.buffer.subarray(length);
        return chunk;
    }
}

function parseTransportSpec(spec) {
    const value = String(spec);
    const colon = value.indexOf(':');
    if (colon <= 0) throw new Error(`invalid transport spec: ${spec}`);
    const scheme = value.slice(0, colon);
    if (scheme !== 'tcp') throw new Error(`TcpTransport only supports tcp:// specs, got: ${scheme}`);
    const rest = value.slice(colon + 1);
    const lastColon = rest.lastIndexOf(':');
    if (lastColon <= 0) throw new Error(`tcp transport spec must be tcp:<host>:<port>`);
    return {
        host: rest.slice(0, lastColon),
        port: Number.parseInt(rest.slice(lastColon + 1), 10),
    };
}

class TcpTransport {
    async connect(transportSpec) {
        const endpoint = parseTransportSpec(transportSpec);
        const socket = net.createConnection(endpoint);
        socket.setNoDelay(true);
        await once(socket, 'connect');

        const reader = new BufferedSocketReader(socket);

        const send = (buf) => new Promise((resolve, reject) => {
            socket.write(buf, (err) => err ? reject(err) : resolve());
        });

        const close = () => new Promise((resolve) => {
            if (socket.destroyed) { resolve(); return; }
            socket.end();
            const timer = setTimeout(() => { if (!socket.destroyed) socket.destroy(); resolve(); }, 250);
            socket.once('close', () => { clearTimeout(timer); resolve(); });
        });

        return {reader, send, socket, close};
    }
}

module.exports = {TcpTransport, BufferedSocketReader};
