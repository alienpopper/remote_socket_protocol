'use strict';

const assert = require('assert');
const net = require('net');
const path = require('path');
const readline = require('readline');
const {spawn} = require('child_process');

const {RSPClient} = require('../client/nodejs/rsp_client');
const {createConnection} = require('../client/nodejs/rsp_net');

const ENDORSEMENT_SUCCESS = 0;
const ETYPE_ACCESS = 'f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b';
const EVALUE_ACCESS_NETWORK = 'f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b';
const ETYPE_ROLE = '0963c0ab-215f-42c1-b042-747bf21e330e';
const EVALUE_ROLE_CLIENT = 'edab2025-4ae1-44f2-a683-1a390586e10c';
const EVALUE_ROLE_RESOURCE_SERVICE = 'a7f8c9d6-3b2e-4f1a-8c9d-5e6f7a8b9c0d';

const HARNESS_TIMEOUT_MS = 45000;
const harnessStartMs = Date.now();

function logDetail(scope, message) {
    const elapsedMs = Date.now() - harnessStartMs;
    console.error(`[nodejs_express_stress][+${elapsedMs}ms][${scope}] ${message}`);
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
            logDetail('fixture:stdout', line);
            try {
                resolve({info: JSON.parse(line), stdoutLines, stderrLines});
            } catch {
            }
        });

        stderrReader.on('line', (line) => {
            stderrLines.push(line);
            logDetail('fixture:stderr', line);
        });

        fixture.once('error', (error) => reject(error));
        fixture.once('exit', (code, signal) => {
            reject(new Error(
                `fixture exited before ready (code=${code}, signal=${signal})\nstdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        });
    });
}

function waitForExpressReady(expressProcess) {
    return new Promise((resolve, reject) => {
        const stdoutLines = [];
        const stderrLines = [];
        const stdoutReader = readline.createInterface({input: expressProcess.stdout});
        const stderrReader = readline.createInterface({input: expressProcess.stderr});

        stdoutReader.on('line', (line) => {
            stdoutLines.push(line);
            logDetail('express:stdout', line);
            if (line.includes('Express over RSP listening on')) {
                resolve({stdoutLines, stderrLines});
            }
        });

        stderrReader.on('line', (line) => {
            stderrLines.push(line);
            logDetail('express:stderr', line);
        });

        expressProcess.once('error', (error) => reject(error));
        expressProcess.once('exit', (code, signal) => {
            reject(new Error(
                `express process exited early (code=${code}, signal=${signal})\nstdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        });
    });
}

async function requestEndorsementOrThrow(client, endorsementNodeId, endorsementType, endorsementValue, label) {
    let reply = null;
    for (let attempt = 0; attempt < 3 && (!reply || reply.status !== ENDORSEMENT_SUCCESS); attempt += 1) {
        reply = await withTimeout(
            client.beginEndorsementRequest(endorsementNodeId, endorsementType, endorsementValue),
            5000,
            `${label} endorsement request`
        );
    }
    if (!reply || reply.status !== ENDORSEMENT_SUCCESS) {
        throw new Error(`${label} endorsement failed (status=${reply ? reply.status : 'null'})`);
    }
}

async function terminateProcess(processHandle) {
    if (!processHandle || processHandle.exitCode !== null || processHandle.killed) {
        return;
    }

    processHandle.kill('SIGTERM');
    await new Promise((resolve) => {
        const timer = setTimeout(() => {
            processHandle.kill('SIGKILL');
            resolve();
        }, 1500);

        processHandle.once('exit', () => {
            clearTimeout(timer);
            resolve();
        });
    });
}

function getFreePort() {
    return new Promise((resolve, reject) => {
        const server = net.createServer();
        server.listen(0, '127.0.0.1', () => {
            const address = server.address();
            if (!address || typeof address === 'string') {
                server.close(() => reject(new Error('failed to allocate test port')));
                return;
            }
            const port = address.port;
            server.close(() => resolve(port));
        });
        server.on('error', reject);
    });
}

function readUntilExpectedWithTimeout(socket, timeoutMs, expectedTokens, label) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        let settled = false;

        const timer = setTimeout(() => {
            cleanup();
            reject(new Error(`${label} timed out after ${timeoutMs}ms`));
        }, timeoutMs);

        const onData = (chunk) => {
            chunks.push(chunk);
            const text = Buffer.concat(chunks).toString('utf8');
            if (expectedTokens.every((token) => text.includes(token))) {
                settled = true;
                cleanup();
                resolve(text);
            }
        };

        const onEndOrClose = () => {
            if (settled) {
                return;
            }
            settled = true;
            cleanup();
            resolve(Buffer.concat(chunks).toString('utf8'));
        };

        const onError = (error) => {
            if (settled) {
                return;
            }
            settled = true;
            cleanup();
            reject(error);
        };

        const cleanup = () => {
            clearTimeout(timer);
            socket.off('data', onData);
            socket.off('end', onEndOrClose);
            socket.off('close', onEndOrClose);
            socket.off('error', onError);
        };

        socket.on('data', onData);
        socket.on('end', onEndOrClose);
        socket.on('close', onEndOrClose);
        socket.on('error', onError);
    });
}

async function sendHttpRequest(client, destinationNodeId, hostPort, requestText, timeoutMs = 12000) {
    const socket = await withTimeout(
        createConnection(client, destinationNodeId, hostPort, {asyncData: true}),
        6000,
        'createConnection'
    );
    const responsePromise = readUntilExpectedWithTimeout(
        socket,
        timeoutMs,
        ['HTTP/1.1'],
        'http response'
    );
    socket.end(requestText);
    const response = await responsePromise;
    socket.destroy();
    return response;
}

async function runConcurrentGets(client, destinationNodeId, hostPort) {
    logDetail('phase', 'concurrent GET flood start');
    const rounds = 5;
    const parallelPerRound = 4;
    for (let round = 0; round < rounds; round += 1) {
        const jobs = Array.from({length: parallelPerRound}, (_, index) => {
            const id = round * parallelPerRound + index;
            const request =
                `GET /?i=${id} HTTP/1.1\r\n` +
                'Host: rsp.local\r\n' +
                'Connection: close\r\n\r\n';
            return sendHttpRequest(client, destinationNodeId, hostPort, request).then((response) => {
                assert.ok(response.includes('HTTP/1.1 200 OK'), `GET ${id} expected HTTP 200`);
            });
        });
        await Promise.all(jobs);
    }
    logDetail('phase', 'concurrent GET flood done');
}

async function runLargeTransferBursts(client, destinationNodeId, hostPort) {
    logDetail('phase', 'large transfer burst start');
    const bodySize = 512 * 1024;
    const body = 'A'.repeat(bodySize);

    const jobs = Array.from({length: 6}, (_, index) => {
        const request =
            'POST /upload HTTP/1.1\r\n' +
            'Host: rsp.local\r\n' +
            `X-Burst-Id: ${index}\r\n` +
            `Content-Length: ${Buffer.byteLength(body)}\r\n` +
            'Content-Type: text/plain\r\n' +
            'Connection: close\r\n\r\n' +
            body;

        return sendHttpRequest(client, destinationNodeId, hostPort, request, 12000).then((response) => {
            assert.ok(response.includes('HTTP/1.1 404 Not Found'), `large POST ${index} expected HTTP 404`);
        });
    });

    await Promise.all(jobs);
    logDetail('phase', 'large transfer burst done');
}

async function runEarlyTerminationTraffic(client, destinationNodeId, hostPort) {
    logDetail('phase', 'early termination traffic start');
    const partialBody = 'B'.repeat(128 * 1024);

    const jobs = Array.from({length: 10}, async (_, index) => {
        const socket = await withTimeout(
            createConnection(client, destinationNodeId, hostPort, {asyncData: true}),
            6000,
            `createConnection partial ${index}`
        );

        socket.on('error', () => {});

        socket.write(
            'POST /partial HTTP/1.1\r\n' +
            'Host: rsp.local\r\n' +
            `X-Partial-Id: ${index}\r\n` +
            'Content-Type: text/plain\r\n' +
            'Content-Length: 262144\r\n' +
            'Connection: keep-alive\r\n\r\n'
        );
        socket.write(partialBody);
        socket.destroy();

        await new Promise((resolve) => {
            socket.once('close', resolve);
            setTimeout(resolve, 250);
        });
    });

    await Promise.all(jobs);

    const probeResponse = await sendHttpRequest(
        client,
        destinationNodeId,
        hostPort,
        'GET /healthz HTTP/1.1\r\nHost: rsp.local\r\nConnection: close\r\n\r\n',
        8000
    );
    assert.ok(probeResponse.includes('HTTP/1.1 200 OK'), 'server should remain healthy after early-terminated clients');
    logDetail('phase', 'early termination traffic done');
}

async function main() {
    const [, , fixturePath] = process.argv;
    if (!fixturePath) {
        throw new Error('Usage: node test/nodejs_express_stress_integration.js <fixture-path>');
    }

    const fixture = spawn(fixturePath, [], {stdio: ['ignore', 'pipe', 'pipe']});
    let expressProcess = null;
    let requesterClient = null;

    try {
        const {info} = await withTimeout(waitForFixtureReady(fixture), 6000, 'fixture readiness');
        const freePort = await getFreePort();
        const hostPort = `127.0.0.1:${freePort}`;
        const appPath = path.resolve(__dirname, '../integration/nodejs_express/working/app.js');

        expressProcess = spawn('node', [appPath], {
            stdio: ['ignore', 'pipe', 'pipe'],
            env: {
                ...process.env,
                RSP_TRANSPORT: info.transport_spec,
                RSP_RESOURCE_SERVICE_NODE_ID: info.resource_service_node_id,
                RSP_ENDORSEMENT_NODE_ID: info.endorsement_service_node_id,
                RSP_HOST_PORT: hostPort,
            },
        });

        await withTimeout(waitForExpressReady(expressProcess), 8000, 'express readiness');

        requesterClient = new RSPClient();
        await requesterClient.connect(info.transport_spec);

        const reachable = await requesterClient.ping(info.endorsement_service_node_id, 3000);
        if (!reachable) {
            throw new Error('endorsement service is unreachable from requester client');
        }

        await requestEndorsementOrThrow(
            requesterClient,
            info.endorsement_service_node_id,
            ETYPE_ACCESS,
            EVALUE_ACCESS_NETWORK,
            'network access'
        );
        await requestEndorsementOrThrow(
            requesterClient,
            info.endorsement_service_node_id,
            ETYPE_ROLE,
            EVALUE_ROLE_CLIENT,
            'client role'
        );
        await requestEndorsementOrThrow(
            requesterClient,
            info.endorsement_service_node_id,
            ETYPE_ROLE,
            EVALUE_ROLE_RESOURCE_SERVICE,
            'resource service role'
        );

        await runConcurrentGets(requesterClient, info.resource_service_node_id, hostPort);
        await runLargeTransferBursts(requesterClient, info.resource_service_node_id, hostPort);
        await runEarlyTerminationTraffic(requesterClient, info.resource_service_node_id, hostPort);

        console.log('nodejs_express_stress_integration passed');
    } finally {
        const closeRequester = requesterClient ? requesterClient.close().catch(() => {}) : Promise.resolve();
        await Promise.all([
            closeRequester,
            terminateProcess(expressProcess),
            terminateProcess(fixture),
        ]);
    }
}

const watchdog = setTimeout(() => {
    console.error(`nodejs_express_stress_integration failed: harness timed out after ${HARNESS_TIMEOUT_MS}ms`);
    process.exit(1);
}, HARNESS_TIMEOUT_MS);

main()
    .then(() => {
        clearTimeout(watchdog);
    })
    .catch((error) => {
        clearTimeout(watchdog);
        console.error(`nodejs_express_stress_integration failed: ${error.message}`);
        process.exit(1);
    });