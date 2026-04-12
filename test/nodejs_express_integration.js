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
const HARNESS_TIMEOUT_MS = 12000;
const harnessStartMs = Date.now();

function logDetail(scope, message) {
    const elapsedMs = Date.now() - harnessStartMs;
    console.error(`[nodejs_express_integration][+${elapsedMs}ms][${scope}] ${message}`);
}

function logPhase(message) {
    logDetail('phase', message);
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
                // wait until fixture prints its JSON ready line
            }
        });

        stderrReader.on('line', (line) => {
            stderrLines.push(line);
            logDetail('fixture:stderr', line);
        });

        fixture.once('error', (error) => {
            logDetail('fixture:event', `error: ${error.message}`);
            reject(error);
        });

        fixture.once('exit', (code, signal) => {
            logDetail('fixture:event', `exit code=${code} signal=${signal}`);
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

        expressProcess.once('error', (error) => {
            logDetail('express:event', `error: ${error.message}`);
            reject(error);
        });

        expressProcess.once('exit', (code, signal) => {
            logDetail('express:event', `exit code=${code} signal=${signal}`);
            reject(new Error(
                `express process exited early (code=${code}, signal=${signal})\nstdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        });

        setTimeout(() => {
            reject(new Error(
                `express process did not become ready in time\nstdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        }, 30000);
    });
}

async function requestEndorsementOrThrow(client, endorsementNodeId, endorsementType, endorsementValue, label) {
    let reply = null;
    for (let attempt = 0; attempt < 3 && (!reply || reply.status !== ENDORSEMENT_SUCCESS); attempt += 1) {
        reply = await withTimeout(
            client.beginEndorsementRequest(endorsementNodeId, endorsementType, endorsementValue),
            3000,
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
        }, 1000);

        processHandle.once('exit', () => {
            clearTimeout(timer);
            resolve();
        });
    });
}

function readUntilExpectedWithTimeout(stream, timeoutMs, expectedTokens) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        let resolved = false;

        const timer = setTimeout(() => {
            cleanup();
            reject(new Error(`timed out waiting for expected HTTP response content after ${timeoutMs}ms`));
        }, timeoutMs);

        const maybeResolve = () => {
            if (resolved) {
                return;
            }

            if (chunks.length === 0) {
                return;
            }

            const text = Buffer.concat(chunks).toString('utf8');
            if (expectedTokens.every((token) => text.includes(token))) {
                resolved = true;
                cleanup();
                resolve(text);
            }
        };

        const onData = (chunk) => {
            chunks.push(chunk);
            maybeResolve();
        };

        const onEnd = () => {
            if (!resolved) {
                resolved = true;
                cleanup();
                resolve(Buffer.concat(chunks).toString('utf8'));
            }
        };

        const onError = (error) => {
            if (!resolved) {
                resolved = true;
                cleanup();
                reject(error);
            }
        };

        const cleanup = () => {
            clearTimeout(timer);
            stream.off('data', onData);
            stream.off('end', onEnd);
            stream.off('error', onError);
        };

        stream.on('data', onData);
        stream.on('end', onEnd);
        stream.on('error', onError);
    });
}

function getFreePort() {
    return new Promise((resolve, reject) => {
        const server = net.createServer();
        server.listen(0, '127.0.0.1', () => {
            const address = server.address();
            if (!address || typeof address === 'string') {
                server.close(() => reject(new Error('failed to allocate a numeric test port')));
                return;
            }

            const port = address.port;
            server.close(() => resolve(port));
        });
        server.on('error', reject);
    });
}

async function main() {
    const [, , fixturePath] = process.argv;
    if (!fixturePath) {
        throw new Error('Usage: node test/nodejs_express_integration.js <fixture-path>');
    }

    const fixture = spawn(fixturePath, [], {stdio: ['ignore', 'pipe', 'pipe']});
    let expressProcess = null;
    let requesterClient = null;

    try {
        logPhase('waiting for fixture readiness');
        const {info} = await withTimeout(waitForFixtureReady(fixture), 5000, 'fixture readiness');
        logPhase('fixture ready');
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

        logPhase('waiting for express readiness');
        await withTimeout(waitForExpressReady(expressProcess), 5000, 'express readiness');
        logPhase('express ready');

        requesterClient = new RSPClient();
        logPhase('connecting requester client');
        await requesterClient.connect(info.transport_spec);
        logPhase('requester client connected');

        logPhase('pinging endorsement service');
        const reachable = await requesterClient.ping(info.endorsement_service_node_id, 3000);
        if (!reachable) {
            throw new Error('endorsement service is unreachable from requester client');
        }
        logPhase('requesting endorsements');

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
        logPhase('endorsements acquired');

        logPhase('opening rsp socket connection');
        const socket = await withTimeout(
            createConnection(requesterClient, info.resource_service_node_id, hostPort, {
                asyncData: true,
            }),
            5000,
            'rsp socket connection'
        );
        logPhase('rsp socket connected; sending HTTP request');
        socket.on('error', (error) => {
            logDetail('requester:socket', `error: ${error.message}`);
        });
        socket.on('close', () => {
            logDetail('requester:socket', 'close');
        });
        socket.on('end', () => {
            logDetail('requester:socket', 'end');
        });

        const responsePromise = readUntilExpectedWithTimeout(socket, 5000, [
            'HTTP/1.1 200 OK',
        ]);
        socket.end('GET / HTTP/1.1\r\nHost: rsp.local\r\nConnection: close\r\n\r\n', (error) => {
            if (error) {
                logDetail('requester:socket', `write callback error: ${error.message}`);
                return;
            }
            logDetail('requester:socket', 'write callback success');
        });

        const response = await responsePromise;
        logPhase('received HTTP response');
        assert.ok(response.includes('HTTP/1.1 200 OK'), 'expected HTTP 200 response through RSP');

        console.log('nodejs_express_integration passed');
    } finally {
        const closeRequester = requesterClient
            ? requesterClient.close().catch(() => {})
            : Promise.resolve();
        await Promise.all([
            closeRequester,
            terminateProcess(expressProcess),
            terminateProcess(fixture),
        ]);
    }
}

const watchdog = setTimeout(() => {
    console.error(`nodejs_express_integration failed: harness timed out after ${HARNESS_TIMEOUT_MS}ms`);
    process.exit(1);
}, HARNESS_TIMEOUT_MS);

main()
    .then(() => {
        clearTimeout(watchdog);
    })
    .catch((error) => {
        clearTimeout(watchdog);
        console.error(`nodejs_express_integration failed: ${error.message}`);
        process.exit(1);
    });
