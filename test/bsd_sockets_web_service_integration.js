'use strict';

// Integration test: RM + BSD sockets RS on this host, Express web service on
// 172.16.206.185:3939.
//
// The test starts the nodejs_ping_fixture (RM + BSD sockets RS in-process),
// endorses an RSP client, then uses createConnection to proxy HTTP requests
// through the BSD sockets RS to the remote Express server.
//
// Usage: node test/bsd_sockets_web_service_integration.js <fixture-path>

const assert = require('assert');
const readline = require('readline');
const {spawn} = require('child_process');

const {RSPClient} = require('../client/nodejs/rsp_client');
const {createConnection} = require('../client/nodejs/rsp_net');

const REMOTE_HOST_PORT = '172.16.206.185:3939';

const ENDORSEMENT_SUCCESS = 0;
const ETYPE_ACCESS = 'f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b';
const EVALUE_ACCESS_NETWORK = 'f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b';
const ETYPE_ROLE = '0963c0ab-215f-42c1-b042-747bf21e330e';
const EVALUE_ROLE_CLIENT = 'edab2025-4ae1-44f2-a683-1a390586e10c';

const HARNESS_TIMEOUT_MS = 30000;
const harnessStartMs = Date.now();

function log(scope, message) {
    const elapsed = Date.now() - harnessStartMs;
    console.error(`[bsd_sockets_web_service][+${elapsed}ms][${scope}] ${message}`);
}

function withTimeout(promise, timeoutMs, label) {
    return Promise.race([
        promise,
        new Promise((_, reject) => {
            setTimeout(() => reject(new Error(`${label} timed out after ${timeoutMs}ms`)), timeoutMs);
        }),
    ]);
}

function waitForFixtureReady(fixture) {
    return new Promise((resolve, reject) => {
        const stdoutLines = [];
        const stderrLines = [];
        const stdoutReader = readline.createInterface({input: fixture.stdout});
        const stderrReader = readline.createInterface({input: fixture.stderr});

        stdoutReader.on('line', (line) => {
            stdoutLines.push(line);
            log('fixture:stdout', line);
            try {
                resolve({info: JSON.parse(line), stdoutLines, stderrLines});
            } catch {
                // wait for the JSON readiness line
            }
        });

        stderrReader.on('line', (line) => {
            stderrLines.push(line);
            log('fixture:stderr', line);
        });

        fixture.once('error', (error) => reject(error));
        fixture.once('exit', (code, signal) => {
            reject(new Error(
                `fixture exited before ready (code=${code}, signal=${signal})\n` +
                `stdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        });
    });
}

async function terminateProcess(proc) {
    if (!proc || proc.exitCode !== null || proc.killed) return;
    proc.kill('SIGTERM');
    await new Promise((resolve) => {
        const timer = setTimeout(() => { proc.kill('SIGKILL'); resolve(); }, 2000);
        proc.once('exit', () => { clearTimeout(timer); resolve(); });
    });
}

async function endorseOrThrow(client, endorsementNodeId, etype, evalue, label) {
    let reply = null;
    for (let attempt = 0; attempt < 3 && (!reply || (reply.status || 0) !== ENDORSEMENT_SUCCESS); attempt += 1) {
        reply = await withTimeout(
            client.beginEndorsementRequest(endorsementNodeId, etype, evalue),
            3000,
            `${label} endorsement attempt ${attempt + 1}`
        );
    }
    if (!reply || (reply.status || 0) !== ENDORSEMENT_SUCCESS) {
        throw new Error(`${label} endorsement failed (status=${reply ? reply.status : 'null'})`);
    }
}

// Read bytes from an RSPSocket until all expectedTokens appear in the
// accumulated text, or until timeoutMs elapses.
function readUntilTokens(socket, timeoutMs, expectedTokens) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        let resolved = false;

        const timer = setTimeout(() => {
            cleanup();
            reject(new Error(`timed out after ${timeoutMs}ms waiting for: ${expectedTokens.join(', ')}`));
        }, timeoutMs);

        const check = () => {
            if (resolved || chunks.length === 0) return;
            const text = Buffer.concat(chunks).toString('utf8');
            if (expectedTokens.every((t) => text.includes(t))) {
                resolved = true;
                cleanup();
                resolve(text);
            }
        };

        const onData = (chunk) => { chunks.push(chunk); check(); };
        const onEnd  = () => {
            if (!resolved) { resolved = true; cleanup(); resolve(Buffer.concat(chunks).toString('utf8')); }
        };
        const onError = (err) => {
            if (!resolved) { resolved = true; cleanup(); reject(err); }
        };
        const cleanup = () => {
            clearTimeout(timer);
            socket.off('data', onData);
            socket.off('end', onEnd);
            socket.off('error', onError);
        };

        socket.on('data', onData);
        socket.on('end', onEnd);
        socket.on('error', onError);
    });
}

// Send a single HTTP/1.0 GET and return the full response text.
async function httpGet(client, rsNodeId, path, timeoutMs = 8000) {
    log('http', `GET ${path} via BSD sockets RS -> ${REMOTE_HOST_PORT}`);
    const socket = await withTimeout(
        createConnection(client, rsNodeId, REMOTE_HOST_PORT, {asyncData: true}),
        timeoutMs,
        `connect to ${REMOTE_HOST_PORT}`
    );

    socket.on('error', (err) => log('socket', `error: ${err.message}`));

    const request = `GET ${path} HTTP/1.0\r\nHost: 172.16.206.185\r\nConnection: close\r\n\r\n`;
    const responsePromise = readUntilTokens(socket, timeoutMs, ['HTTP/1.']);
    socket.end(request);

    const response = await responsePromise;
    log('http', `${path} -> ${response.split('\r\n')[0]}`);
    return response;
}

async function main() {
    const [,, fixturePath] = process.argv;
    if (!fixturePath) {
        throw new Error('Usage: node test/bsd_sockets_web_service_integration.js <fixture-path>');
    }

    const fixture = spawn(fixturePath, [], {stdio: ['ignore', 'pipe', 'pipe']});
    let client = null;

    try {
        log('phase', 'waiting for fixture readiness');
        const {info} = await withTimeout(waitForFixtureReady(fixture), 10000, 'fixture readiness');
        log('phase', `fixture ready – transport=${info.transport_spec} rs=${info.resource_service_node_id}`);

        client = new RSPClient();
        await client.connect(info.transport_spec);
        log('phase', 'client connected to RM');

        // Ping the endorsement service to ensure the route through the RM is
        // established before we send any endorsement requests.
        log('phase', 'pinging endorsement service');
        const reachable = await withTimeout(
            client.ping(info.endorsement_service_node_id, 3000),
            5000,
            'endorsement service ping'
        );
        if (!reachable) {
            throw new Error('endorsement service is unreachable from client');
        }
        log('phase', 'endorsement service reachable');

        // Endorse this client so the BSD sockets RS will accept its requests.
        log('phase', 'requesting endorsements');
        await endorseOrThrow(client, info.endorsement_service_node_id, ETYPE_ACCESS, EVALUE_ACCESS_NETWORK, 'network access');
        await endorseOrThrow(client, info.endorsement_service_node_id, ETYPE_ROLE, EVALUE_ROLE_CLIENT, 'client role');
        log('phase', 'endorsements acquired');

        const rsNodeId = info.resource_service_node_id;

        // ── Test 1: home page returns HTML ────────────────────────────────────
        log('phase', 'test 1 – home page HTML');
        const homeResponse = await httpGet(client, rsNodeId, '/');
        assert.ok(homeResponse.includes('HTTP/1.'), 'expected HTTP response');
        assert.ok(homeResponse.includes('200'), 'expected HTTP 200 for /');
        assert.ok(homeResponse.includes('RSP Web Service'), 'expected page title in /');
        log('phase', 'test 1 passed');

        // ── Test 2: /healthz returns JSON with ok:true ─────────────────────
        log('phase', 'test 2 – /healthz JSON');
        const healthResponse = await httpGet(client, rsNodeId, '/healthz');
        assert.ok(healthResponse.includes('200'), 'expected HTTP 200 for /healthz');
        assert.ok(healthResponse.includes('"ok":true'), 'expected ok:true in /healthz');
        assert.ok(healthResponse.includes('rsp-web-service'), 'expected service name in /healthz');
        log('phase', 'test 2 passed');

        // ── Test 3: /api/products returns JSON array ──────────────────────
        log('phase', 'test 3 – /api/products JSON');
        const productsResponse = await httpGet(client, rsNodeId, '/api/products');
        assert.ok(productsResponse.includes('200'), 'expected HTTP 200 for /api/products');
        assert.ok(productsResponse.includes('"products"'), 'expected products array');
        assert.ok(productsResponse.includes('Widget Alpha'), 'expected product name in /api/products');
        log('phase', 'test 3 passed');

        // ── Test 4: /api/users returns user list ─────────────────────────
        log('phase', 'test 4 – /api/users JSON');
        const usersResponse = await httpGet(client, rsNodeId, '/api/users');
        assert.ok(usersResponse.includes('200'), 'expected HTTP 200 for /api/users');
        assert.ok(usersResponse.includes('"users"'), 'expected users array');
        assert.ok(usersResponse.includes('alice'), 'expected user alice in /api/users');
        log('phase', 'test 4 passed');

        // ── Test 5: /products page returns HTML table ─────────────────────
        log('phase', 'test 5 – /products HTML page');
        const productsPageResponse = await httpGet(client, rsNodeId, '/products');
        assert.ok(productsPageResponse.includes('200'), 'expected HTTP 200 for /products');
        assert.ok(productsPageResponse.includes('Product Catalogue'), 'expected heading in /products');
        log('phase', 'test 5 passed');

        // ── Test 6: unknown path returns 404 ─────────────────────────────
        log('phase', 'test 6 – 404 for unknown path');
        const notFoundResponse = await httpGet(client, rsNodeId, '/no-such-page-xyz');
        assert.ok(notFoundResponse.includes('404'), 'expected HTTP 404 for unknown path');
        log('phase', 'test 6 passed');

        console.log('bsd_sockets_web_service_integration passed');
    } finally {
        if (client) await client.close().catch(() => {});
        await terminateProcess(fixture);
    }
}

const watchdog = setTimeout(() => {
    console.error(`bsd_sockets_web_service_integration failed: harness timed out after ${HARNESS_TIMEOUT_MS}ms`);
    process.exit(1);
}, HARNESS_TIMEOUT_MS);

main()
    .then(() => clearTimeout(watchdog))
    .catch((error) => {
        clearTimeout(watchdog);
        console.error(`bsd_sockets_web_service_integration failed: ${error.message}`);
        process.exit(1);
    });
