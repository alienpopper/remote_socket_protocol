'use strict';

// RSP socket abstraction layer for Node.js.
//
// Provides RSPSocket and RSPServer as streaming replacements for the BSD sockets
// the C++ client creates. RSPSocket is a Duplex stream compatible with the Node.js
// net module API. RSPServer is an EventEmitter that behaves like net.Server.
//
// Usage:
//   const {createConnection, createServer} = require('./rsp_net');
//
//   // Server side
//   const server = await createServer(client, rmNodeId, '127.0.0.1:8080');
//   server.on('connection', (socket) => {
//       socket.on('data', (chunk) => socket.write(chunk)); // echo
//   });
//
//   // Client side
//   const socket = await createConnection(client, rmNodeId, '127.0.0.1:8080');
//   socket.write('hello');
//   socket.on('data', (chunk) => console.log(chunk.toString()));

const {Duplex, EventEmitter} = require('stream');
const messages = require('./messages');

const {SUCCESS, SOCKET_DATA, SOCKET_CLOSED, SOCKET_ERROR, NEW_CONNECTION} = messages.SOCKET_STATUS;
const TRACE = process.env.RSP_NET_TRACE === '1';

function trace(message) {
    if (TRACE) {
        console.error(`[rsp_net] ${message}`);
    }
}

// RSPSocket wraps a single RSP socket ID as a Node.js Duplex stream.
//
// Data received from the remote end is pushed as Buffer chunks via the readable
// side. Data written to the writable side is sent via the RSP socket. The socket
// is closed when the stream is ended or destroyed.
class RSPSocket extends Duplex {
    constructor(client, socketId) {
        super({allowHalfOpen: false});
        this._client = client;
        this._socketId = socketId;
        this._closing = false;

        client.attachSocketHandler(socketId, (reply) => this._onSocketReply(reply));
    }

    get socketId() {
        return this._socketId;
    }

    _onSocketReply(reply) {
        const status = reply.error || 0;
        trace(`socket=${this._socketId} reply_status=${status}`);
        if (status === SOCKET_DATA) {
            const data = reply.data ? Buffer.from(reply.data, 'hex') : Buffer.alloc(0);
            trace(`socket=${this._socketId} data_len=${data.length}`);
            this.push(data);
        } else if (status === SOCKET_CLOSED || status === SOCKET_ERROR) {
            this._closing = true;
            this._client.detachSocketHandler(this._socketId);
            this.push(null);
        }
        // SUCCESS replies from fire-and-forget sends are silently ignored.
    }

    _write(chunk, encoding, callback) {
        if (this._closing) {
            callback(new Error('socket is closed'));
            return;
        }
        const data = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk, encoding);
        trace(`socket=${this._socketId} write_len=${data.length}`);
        this._client.socketSend(this._socketId, data)
            .then((sent) => {
                if (!sent) {
                    callback(new Error(`socket send failed for socket ${this._socketId}`));
                    return;
                }
                callback();
            })
            .catch(callback);
    }

    _read(_size) {
        // Data is pushed asynchronously from _onSocketReply; nothing to pull here.
    }

    _final(callback) {
        // Preserve half-close semantics: allow reading remote response data
        // after local writes end (e.g., HTTP request followed by response).
        callback();
    }

    _destroy(err, callback) {
        this._doClose().catch(() => {}).finally(() => callback(err));
    }

    _doClose() {
        if (this._closing) return Promise.resolve();
        this._closing = true;
        this._client.detachSocketHandler(this._socketId);
        return this._client.socketClose(this._socketId)
            .then((closed) => {
                if (!closed) {
                    trace(`socket=${this._socketId} close not acknowledged`);
                }
            });
    }
}

// RSPServer wraps a listening RSP socket and emits 'connection' events with
// RSPSocket instances for each incoming connection.
//
// The server uses async-accept mode: the resource manager delivers NEW_CONNECTION
// replies automatically without requiring explicit acceptTCP calls.
class RSPServer extends EventEmitter {
    constructor(client, nodeId, listenSocketId) {
        super();
        this._client = client;
        this._nodeId = nodeId;
        this._listenSocketId = listenSocketId;
        this._closed = false;

        client.attachSocketHandler(listenSocketId, (reply) => this._onListenReply(reply));
    }

    get socketId() {
        return this._listenSocketId;
    }

    _onListenReply(reply) {
        const status = reply.error || 0;
        if (status === NEW_CONNECTION && reply.new_socket_id?.value) {
            // _handleSocketReply already registered the route for new_socket_id.
            const socket = new RSPSocket(this._client, reply.new_socket_id.value);
            this.emit('connection', socket);
        } else if (status === SOCKET_CLOSED || status === SOCKET_ERROR) {
            if (!this._closed) {
                this._closed = true;
                this._client.detachSocketHandler(this._listenSocketId);
                this.emit('close');
            }
        }
    }

    async close() {
        if (this._closed) return;
        this._closed = true;
        this._client.detachSocketHandler(this._listenSocketId);
        await this._client.socketClose(this._listenSocketId).catch(() => {});
    }
}

// Create an RSPSocket connected to hostPort via the given RSP node.
// Returns a connected RSPSocket ready for reading and writing.
async function createConnection(client, nodeId, hostPort, options = {}) {
    const reply = await client.connectTCPEx(nodeId, hostPort, {asyncData: true, ...options});
    const status = reply?.error ?? SUCCESS;
    if (status !== SUCCESS || !reply?.socket_id?.value) {
        throw Object.assign(new Error(`RSP connect failed (status=${status})`), {status, reply});
    }
    return new RSPSocket(client, reply.socket_id.value);
}

// Create an RSPServer listening on hostPort via the given RSP node.
// Returns an RSPServer that emits 'connection' for each incoming RSPSocket.
async function createServer(client, nodeId, hostPort, options = {}) {
    const reply = await client.listenTCPEx(nodeId, hostPort, {
        asyncAccept: true,
        childrenAsyncData: true,
        ...options,
    });
    const status = reply?.error ?? SUCCESS;
    if (status !== SUCCESS || !reply?.socket_id?.value) {
        throw Object.assign(new Error(`RSP listen failed (status=${status})`), {status, reply});
    }
    return new RSPServer(client, nodeId, reply.socket_id.value);
}

module.exports = {RSPSocket, RSPServer, createConnection, createServer};
