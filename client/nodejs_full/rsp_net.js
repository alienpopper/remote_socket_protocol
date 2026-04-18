'use strict';

// RSP socket abstraction layer for the full Node.js client.
//
// Identical API to client/nodejs/rsp_net.js; imports RSPClient from this package.
// RSPSocket is a Duplex stream compatible with the Node.js net module API.
// RSPServer is an EventEmitter that emits 'connection' with RSPSocket instances.
//
// Usage:
//   const {createConnection, createServer} = require('./rsp_net');
//
//   const server = await createServer(client, rmNodeId, '127.0.0.1:8080');
//   server.on('connection', (socket) => {
//       socket.on('data', (chunk) => socket.write(chunk));
//   });
//
//   const socket = await createConnection(client, rmNodeId, '127.0.0.1:8080');
//   socket.write('hello');
//   socket.on('data', (chunk) => console.log(chunk.toString()));

const {Duplex, EventEmitter} = require('stream');

// Rely on messages from the base client (minimal messages.js is still used for
// stream status constants; the full client doesn't change the wire protocol).
const {STREAM_STATUS} = require('../nodejs/messages');
const {SUCCESS, STREAM_DATA, STREAM_CLOSED, STREAM_ERROR, NEW_CONNECTION} = STREAM_STATUS;

const TRACE = process.env.RSP_NET_TRACE === '1';

function trace(msg) {
    if (TRACE) console.error(`[rsp_net_full] ${msg}`);
}

class RSPSocket extends Duplex {
    constructor(client, socketId) {
        super({allowHalfOpen: true});
        this._client = client;
        this._socketId = socketId;
        this._closing = false;
        client.attachStreamHandler(socketId, (reply) => this._onStreamReply(reply));
    }

    get socketId() {
        return this._socketId;
    }

    _onStreamReply(reply) {
        const status = reply.error || 0;
        trace(`socket=${this._socketId} status=${status}`);
        if (status === STREAM_DATA) {
            const data = reply.data ? Buffer.from(reply.data, 'base64') : Buffer.alloc(0);
            trace(`socket=${this._socketId} data_len=${data.length}`);
            this.push(data);
        } else if (status === STREAM_CLOSED || status === STREAM_ERROR) {
            this._closing = true;
            this._client.detachStreamHandler(this._socketId);
            this.push(null);
        }
    }

    _write(chunk, encoding, callback) {
        if (this._closing) { callback(new Error('socket is closed')); return; }
        const data = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk, encoding);
        trace(`socket=${this._socketId} write_len=${data.length}`);
        this._client.streamSend(this._socketId, data)
            .then((sent) => sent ? callback() : callback(new Error(`socket send failed for ${this._socketId}`)))
            .catch(callback);
    }

    _read(_size) { /* data pushed asynchronously from _onStreamReply */ }

    _final(callback) { callback(); }

    _destroy(err, callback) {
        this._doClose().catch(() => {}).finally(() => callback(err));
    }

    _doClose() {
        if (this._closing) return Promise.resolve();
        this._closing = true;
        this._client.detachStreamHandler(this._socketId);
        return this._client.streamClose(this._socketId).then((closed) => {
            if (!closed) trace(`socket=${this._socketId} close not acknowledged`);
        });
    }
}

class RSPServer extends EventEmitter {
    constructor(client, nodeId, listenSocketId) {
        super();
        this._client = client;
        this._nodeId = nodeId;
        this._listenSocketId = listenSocketId;
        this._closed = false;
        client.attachStreamHandler(listenSocketId, (reply) => this._onListenReply(reply));
    }

    get socketId() {
        return this._listenSocketId;
    }

    _onListenReply(reply) {
        const status = reply.error || 0;
        if (status === NEW_CONNECTION && reply.new_stream_id?.value) {
            const socket = new RSPSocket(this._client, reply.new_stream_id.value);
            this.emit('connection', socket);
        } else if (status === STREAM_CLOSED || status === STREAM_ERROR) {
            if (!this._closed) {
                this._closed = true;
                this._client.detachStreamHandler(this._listenSocketId);
                this.emit('close');
            }
        }
    }

    async close() {
        if (this._closed) return;
        this._closed = true;
        this._client.detachStreamHandler(this._listenSocketId);
        await this._client.streamClose(this._listenSocketId).catch(() => {});
    }
}

async function createConnection(client, nodeId, hostPort, options = {}) {
    const reply = await client.connectTCPEx(nodeId, hostPort, {asyncData: true, ...options});
    const status = reply?.error ?? SUCCESS;
    if (status !== SUCCESS || !reply?.stream_id?.value) {
        throw Object.assign(new Error(`RSP connect failed (status=${status})`), {status, reply});
    }
    return new RSPSocket(client, reply.stream_id.value);
}

async function createServer(client, nodeId, hostPort, options = {}) {
    const reply = await client.listenTCPEx(nodeId, hostPort, {asyncAccept: true, childrenAsyncData: true, ...options});
    const status = reply?.error ?? SUCCESS;
    if (status !== SUCCESS || !reply?.stream_id?.value) {
        throw Object.assign(new Error(`RSP listen failed (status=${status})`), {status, reply});
    }
    return new RSPServer(client, nodeId, reply.stream_id.value);
}

module.exports = {RSPSocket, RSPServer, createConnection, createServer};
