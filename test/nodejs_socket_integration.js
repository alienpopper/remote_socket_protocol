'use strict';

// Socket integration tests for the Node.js RSP client.
//
// Requires the nodejs_ping_fixture binary (built from test/nodejs_ping_fixture.cpp).
// The fixture starts a resource manager with TCP transport and prints a JSON
// startup line with transport_spec and node IDs.
//
// Usage: node test/nodejs_socket_integration.js <fixture-path>

const assert = require('assert');
const readline = require('readline');
const net = require('net');
const {spawn} = require('child_process');
const {pipeline} = require('stream');

const {RSPClient} = require('../client/nodejs/rsp_client');
const {createConnection, createServer} = require('../client/nodejs/rsp_net');

// --- Fixture helpers ---

function waitForFixtureReady(fixture) {
    return new Promise((resolve, reject) => {
        const stdoutLines = [];
        const stderrLines = [];
        const stdoutReader = readline.createInterface({input: fixture.stdout});
        const stderrReader = readline.createInterface({input: fixture.stderr});

        const cleanup = () => { stdoutReader.close(); stderrReader.close(); };

        stdoutReader.on('line', (line) => {
            stdoutLines.push(line);
            try {
                cleanup();
                resolve({info: JSON.parse(line), stdoutLines, stderrLines});
            } catch {
                // keep waiting for a parseable line
            }
        });

        stderrReader.on('line', (line) => stderrLines.push(line));

        fixture.once('error', (err) => { cleanup(); reject(err); });
        fixture.once('exit', (code, signal) => {
            cleanup();
            reject(new Error(
                `fixture exited before ready (code=${code}, signal=${signal})\n` +
                `stdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        });
    });
}

async function terminateFixture(fixture) {
    if (fixture.exitCode !== null || fixture.killed) return;
    fixture.kill('SIGTERM');
    await new Promise((resolve) => {
        const timer = setTimeout(() => { fixture.kill('SIGKILL'); resolve(); }, 5000);
        fixture.once('exit', () => { clearTimeout(timer); resolve(); });
    });
}

function getFreePort() {
    return new Promise((resolve, reject) => {
        const server = net.createServer();
        server.listen(0, '127.0.0.1', () => {
            const port = server.address().port;
            server.close(() => resolve(port));
        });
        server.on('error', reject);
    });
}

// Read all data from a stream until it ends, resolving with a Buffer.
function readAll(stream) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        stream.on('data', (c) => chunks.push(c));
        stream.on('end', () => resolve(Buffer.concat(chunks)));
        stream.on('error', reject);
    });
}

// Wait for the first 'data' event and return the chunk.
function readChunk(stream) {
    return new Promise((resolve, reject) => {
        stream.once('data', resolve);
        stream.once('error', reject);
    });
}

// --- Test runner ---

const results = [];

async function runTest(name, fn) {
    try {
        await fn();
        results.push({name, passed: true});
        console.log(`  PASS  ${name}`);
    } catch (err) {
        results.push({name, passed: false, error: err.message});
        console.log(`  FAIL  ${name}: ${err.message}`);
    }
}

// --- Tests ---

async function testPingViaRSPClient(info) {
    const client = new RSPClient();
    try {
        await client.connect(info.transport_spec);
        const ok = await client.ping(info.resource_manager_node_id);
        assert.strictEqual(ok, true, 'ping to resource manager should succeed');
    } finally {
        await client.close().catch(() => {});
    }
}

async function testPingTimeout(info) {
    const client = new RSPClient();
    try {
        await client.connect(info.transport_spec);
        // Use a non-existent node ID; ping should return false (timeout), not throw.
        const fakeNodeId = '00000000-0000-0000-0000-000000000001';
        const ok = await client.ping(fakeNodeId, 500);
        assert.strictEqual(ok, false, 'ping to unknown node should return false on timeout');
    } finally {
        await client.close().catch(() => {});
    }
}

async function testSocketSendReceive(info) {
    const port = await getFreePort();
    const hostPort = `127.0.0.1:${port}`;
    const rmNodeId = info.resource_manager_node_id;

    const clientA = new RSPClient();
    const clientB = new RSPClient();
    try {
        await Promise.all([
            clientA.connect(info.transport_spec),
            clientB.connect(info.transport_spec),
        ]);

        const server = await createServer(clientA, rmNodeId, hostPort);

        // Collect the first incoming connection from the server.
        const serverSocketPromise = new Promise((resolve) => server.once('connection', resolve));

        const clientSocket = await createConnection(clientB, rmNodeId, hostPort);
        const serverSocket = await serverSocketPromise;

        const testData = Buffer.from('hello from RSP socket', 'utf8');
        clientSocket.write(testData);

        const received = await readChunk(serverSocket);
        assert.ok(received.equals(testData), `expected "${testData}" but got "${received}"`);

        await Promise.all([
            clientSocket.end() && new Promise((r) => clientSocket.once('close', r)),
            serverSocket.end() && new Promise((r) => serverSocket.once('close', r)),
        ]);
        await server.close();
    } finally {
        await Promise.all([clientA.close(), clientB.close()].map((p) => p.catch(() => {})));
    }
}

async function testBidirectionalDataExchange(info) {
    const port = await getFreePort();
    const hostPort = `127.0.0.1:${port}`;
    const rmNodeId = info.resource_manager_node_id;

    const clientA = new RSPClient();
    const clientB = new RSPClient();
    try {
        await Promise.all([
            clientA.connect(info.transport_spec),
            clientB.connect(info.transport_spec),
        ]);

        const server = await createServer(clientA, rmNodeId, hostPort);
        const serverSocketPromise = new Promise((resolve) => server.once('connection', resolve));
        const clientSocket = await createConnection(clientB, rmNodeId, hostPort);
        const serverSocket = await serverSocketPromise;

        const msgAtoB = Buffer.from('ping from client', 'utf8');
        const msgBtoA = Buffer.from('pong from server', 'utf8');

        // Client → Server
        clientSocket.write(msgAtoB);
        const receivedByServer = await readChunk(serverSocket);
        assert.ok(receivedByServer.equals(msgAtoB), 'server should receive client data');

        // Server → Client
        serverSocket.write(msgBtoA);
        const receivedByClient = await readChunk(clientSocket);
        assert.ok(receivedByClient.equals(msgBtoA), 'client should receive server data');

        await Promise.all([
            clientSocket.end() && new Promise((r) => clientSocket.once('close', r)),
            serverSocket.end() && new Promise((r) => serverSocket.once('close', r)),
        ]);
        await server.close();
    } finally {
        await Promise.all([clientA.close(), clientB.close()].map((p) => p.catch(() => {})));
    }
}

async function testMultipleConnections(info) {
    const port = await getFreePort();
    const hostPort = `127.0.0.1:${port}`;
    const rmNodeId = info.resource_manager_node_id;
    const CONNECTION_COUNT = 3;

    const clientA = new RSPClient();
    const clientB = new RSPClient();
    try {
        await Promise.all([
            clientA.connect(info.transport_spec),
            clientB.connect(info.transport_spec),
        ]);

        const server = await createServer(clientA, rmNodeId, hostPort);

        // Collect server-side sockets as connections arrive.
        const serverSockets = [];
        server.on('connection', (s) => serverSockets.push(s));

        // Open multiple client connections.
        const clientSockets = await Promise.all(
            Array.from({length: CONNECTION_COUNT}, () => createConnection(clientB, rmNodeId, hostPort))
        );

        // Wait until server has all connections.
        await new Promise((resolve, reject) => {
            const deadline = setTimeout(() => reject(new Error('timed out waiting for connections')), 5000);
            const check = () => {
                if (serverSockets.length >= CONNECTION_COUNT) { clearTimeout(deadline); resolve(); }
                else setTimeout(check, 20);
            };
            check();
        });

        assert.strictEqual(serverSockets.length, CONNECTION_COUNT, `expected ${CONNECTION_COUNT} connections`);

        // Echo test on each connection in parallel.
        await Promise.all(clientSockets.map(async (sock, i) => {
            const msg = Buffer.from(`connection-${i}`, 'utf8');
            sock.write(msg);
            const rx = await readChunk(serverSockets[i]);
            assert.ok(rx.equals(msg), `connection ${i}: data mismatch`);
        }));

        // Close everything.
        await Promise.all([
            ...clientSockets.map((s) => s.end() && new Promise((r) => s.once('close', r))),
            ...serverSockets.map((s) => s.end() && new Promise((r) => s.once('close', r))),
        ]);
        await server.close();
    } finally {
        await Promise.all([clientA.close(), clientB.close()].map((p) => p.catch(() => {})));
    }
}

async function testStreamPipeline(info) {
    const port = await getFreePort();
    const hostPort = `127.0.0.1:${port}`;
    const rmNodeId = info.resource_manager_node_id;

    const clientA = new RSPClient();
    const clientB = new RSPClient();
    try {
        await Promise.all([
            clientA.connect(info.transport_spec),
            clientB.connect(info.transport_spec),
        ]);

        // Server echoes data back.
        const server = await createServer(clientA, rmNodeId, hostPort);
        server.on('connection', (serverSocket) => {
            pipeline(serverSocket, serverSocket, (err) => {
                if (err && err.code !== 'ERR_STREAM_DESTROYED') {
                    console.error('echo pipeline error:', err.message);
                }
            });
        });

        const clientSocket = await createConnection(clientB, rmNodeId, hostPort);
        const chunks = [];
        const doneReading = new Promise((resolve, reject) => {
            clientSocket.on('data', (c) => chunks.push(c));
            clientSocket.on('end', resolve);
            clientSocket.on('error', reject);
        });

        const payload = Buffer.from('stream pipeline test payload', 'utf8');
        clientSocket.end(payload);
        await doneReading;

        const result = Buffer.concat(chunks);
        assert.ok(result.equals(payload), `echo mismatch: got "${result}"`);

        await server.close();
    } finally {
        await Promise.all([clientA.close(), clientB.close()].map((p) => p.catch(() => {})));
    }
}

async function testConnectRefused(info) {
    const client = new RSPClient();
    try {
        await client.connect(info.transport_spec);
        // Port 1 is almost universally unreachable / refused.
        await assert.rejects(
            () => createConnection(client, info.resource_manager_node_id, '127.0.0.1:1'),
            /RSP connect failed/,
            'connecting to a refused port should throw'
        );
    } finally {
        await client.close().catch(() => {});
    }
}

// --- Main ---

async function main() {
    const [, , fixturePath] = process.argv;
    if (!fixturePath) {
        console.error('Usage: node test/nodejs_socket_integration.js <fixture-path>');
        process.exit(1);
    }

    const fixture = spawn(fixturePath, [], {stdio: ['ignore', 'pipe', 'pipe']});
    let info;
    try {
        ({info} = await waitForFixtureReady(fixture));
    } catch (err) {
        console.error(`fixture failed to start: ${err.message}`);
        process.exit(1);
    }

    console.log('Running nodejs socket integration tests...');

    await runTest('ping returns true on success', () => testPingViaRSPClient(info));
    await runTest('ping returns false on timeout', () => testPingTimeout(info));
    await runTest('socket send and receive', () => testSocketSendReceive(info));
    await runTest('bidirectional data exchange', () => testBidirectionalDataExchange(info));
    await runTest('multiple simultaneous connections', () => testMultipleConnections(info));
    await runTest('stream pipeline (echo)', () => testStreamPipeline(info));
    await runTest('connect refused throws', () => testConnectRefused(info));

    await terminateFixture(fixture);

    const passed = results.filter((r) => r.passed).length;
    const failed = results.filter((r) => !r.passed).length;
    console.log(`\n${passed} passed, ${failed} failed`);

    if (failed > 0) {
        process.exit(1);
    }
    console.log('nodejs_socket_integration passed');
}

main().catch((err) => {
    console.error(`nodejs_socket_integration failed: ${err.message}`);
    process.exit(1);
});
