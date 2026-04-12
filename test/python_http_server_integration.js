'use strict';

const assert = require('assert');
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
const HARNESS_TIMEOUT_MS = 20000;

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
            try {
                resolve({info: JSON.parse(line), stdoutLines, stderrLines});
            } catch {
            }
        });

        stderrReader.on('line', (line) => {
            stderrLines.push(line);
            console.error(`[python_http_server_integration][fixture] ${line}`);
        });

        fixture.once('error', (error) => reject(error));
        fixture.once('exit', (code, signal) => {
            reject(new Error(
                `fixture exited before ready (code=${code}, signal=${signal})\nstdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        });
    });
}

function waitForPythonServerReady(proc) {
    return new Promise((resolve, reject) => {
        const stdoutReader = readline.createInterface({input: proc.stdout});
        const stderrLines = [];
        const stderrReader = readline.createInterface({input: proc.stderr});

        stdoutReader.on('line', (line) => {
            if (line.includes('Python HTTP server over RSP listening on')) {
                resolve();
            }
        });

        stderrReader.on('line', (line) => {
            stderrLines.push(line);
            console.error(`[python_http_server_integration][python] ${line}`);
        });

        proc.once('error', (error) => reject(error));
        proc.once('exit', (code, signal) => {
            reject(new Error(
                `python server exited before ready (code=${code}, signal=${signal})\nstderr:\n${stderrLines.join('\n')}`
            ));
        });
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

function readUntilExpectedWithTimeout(stream, timeoutMs, expectedTokens) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        let resolved = false;

        const timer = setTimeout(() => {
            cleanup();
            reject(new Error(`timed out waiting for expected HTTP response content after ${timeoutMs}ms`));
        }, timeoutMs);

        const maybeResolve = () => {
            if (resolved || chunks.length === 0) {
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

async function terminateProcess(proc) {
    if (!proc || proc.exitCode !== null || proc.killed) {
        return;
    }
    proc.kill('SIGTERM');
    await new Promise((resolve) => {
        const timer = setTimeout(() => {
            proc.kill('SIGKILL');
            resolve();
        }, 1500);
        proc.once('exit', () => {
            clearTimeout(timer);
            resolve();
        });
    });
}

async function main() {
    const [, , fixturePath] = process.argv;
    if (!fixturePath) {
        throw new Error('Usage: node test/python_http_server_integration.js <fixture-path>');
    }

    const fixture = spawn(fixturePath, [], {stdio: ['ignore', 'pipe', 'pipe']});
    let pythonServer = null;
    let requesterClient = null;

    try {
        const {info} = await withTimeout(waitForFixtureReady(fixture), 10000, 'fixture readiness');

        const appPath = path.resolve(__dirname, '../integration/python_http_server/working/app.py');
        const pythonBin = path.resolve(__dirname, '../.venv/bin/python');
        const hostPort = '127.0.0.1:51880';

        pythonServer = spawn(pythonBin, [appPath], {
            stdio: ['ignore', 'pipe', 'pipe'],
            env: {
                ...process.env,
                RSP_TRANSPORT: info.transport_spec,
                RSP_RESOURCE_SERVICE_NODE_ID: info.resource_service_node_id,
                RSP_ENDORSEMENT_NODE_ID: info.endorsement_service_node_id,
                RSP_HOST_PORT: hostPort,
            },
        });

        await withTimeout(waitForPythonServerReady(pythonServer), 15000, 'python server readiness');

        requesterClient = new RSPClient();
        await withTimeout(requesterClient.connect(info.transport_spec), 5000, 'requester connect');

        const reachable = await withTimeout(requesterClient.ping(info.endorsement_service_node_id, 3000), 4000, 'endorsement ping');
        if (!reachable) {
            throw new Error('endorsement service unreachable from requester client');
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

        const socket = await withTimeout(
            createConnection(requesterClient, info.resource_service_node_id, hostPort, {asyncData: true}),
            6000,
            'rsp socket connection'
        );

        const responsePromise = readUntilExpectedWithTimeout(socket, 6000, [
            'HTTP/1.1 200 OK',
            'Remote Socket Protocol',
        ]);

        socket.write('GET / HTTP/1.1\r\nHost: python-rsp.local\r\nConnection: close\r\n\r\n');
        const response = await responsePromise;
        assert.ok(response.includes('HTTP/1.1 200 OK'));

        console.log('python_http_server_integration passed');
    } finally {
        const closeRequester = requesterClient ? requesterClient.close().catch(() => {}) : Promise.resolve();
        await Promise.all([
            closeRequester,
            terminateProcess(pythonServer),
            terminateProcess(fixture),
        ]);
    }
}

const watchdog = setTimeout(() => {
    console.error(`python_http_server_integration failed: harness timed out after ${HARNESS_TIMEOUT_MS}ms`);
    process.exit(1);
}, HARNESS_TIMEOUT_MS);

main()
    .then(() => clearTimeout(watchdog))
    .catch((error) => {
        clearTimeout(watchdog);
        console.error(`python_http_server_integration failed: ${error.message}`);
        process.exit(1);
    });
