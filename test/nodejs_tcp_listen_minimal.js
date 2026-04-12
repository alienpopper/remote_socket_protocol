'use strict';

const assert = require('assert');
const net = require('net');
const readline = require('readline');
const {spawn} = require('child_process');

const {RSPClient} = require('../client/nodejs/rsp_client');
const {createConnection, createServer} = require('../client/nodejs/rsp_net');

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

        const cleanup = () => {
            stdoutReader.close();
            stderrReader.close();
        };

        stdoutReader.on('line', (line) => {
            stdoutLines.push(line);
            try {
                cleanup();
                resolve({info: JSON.parse(line), stdoutLines, stderrLines});
            } catch {
                // wait for fixture readiness JSON line
            }
        });

        stderrReader.on('line', (line) => stderrLines.push(line));

        fixture.once('error', (error) => {
            cleanup();
            reject(error);
        });

        fixture.once('exit', (code, signal) => {
            cleanup();
            reject(new Error(
                `fixture exited before ready (code=${code}, signal=${signal})\nstdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        });
    });
}

function getFreePort() {
    return new Promise((resolve, reject) => {
        const server = net.createServer();
        server.listen(0, '127.0.0.1', () => {
            const address = server.address();
            if (!address || typeof address === 'string') {
                server.close(() => reject(new Error('failed to allocate local port')));
                return;
            }

            server.close(() => resolve(address.port));
        });
        server.on('error', reject);
    });
}

function readChunkWithTimeout(stream, timeoutMs, label) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            cleanup();
            reject(new Error(`${label} timed out after ${timeoutMs}ms`));
        }, timeoutMs);

        const onData = (chunk) => {
            cleanup();
            resolve(chunk);
        };

        const onError = (error) => {
            cleanup();
            reject(error);
        };

        const cleanup = () => {
            clearTimeout(timer);
            stream.off('data', onData);
            stream.off('error', onError);
        };

        stream.on('data', onData);
        stream.on('error', onError);
    });
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
        }, 5000);

        processHandle.once('exit', () => {
            clearTimeout(timer);
            resolve();
        });
    });
}

async function main() {
    const [, , fixturePath] = process.argv;
    if (!fixturePath) {
        throw new Error('Usage: node test/nodejs_tcp_listen_minimal.js <fixture-path>');
    }

    const fixture = spawn(fixturePath, [], {stdio: ['ignore', 'pipe', 'pipe']});
    const listenerClient = new RSPClient();
    const connectorClient = new RSPClient();

    let rspServer = null;
    let clientSocket = null;
    let acceptedSocket = null;

    try {
        const {info} = await withTimeout(waitForFixtureReady(fixture), 5000, 'fixture readiness');

        await withTimeout(Promise.all([
            listenerClient.connect(info.transport_spec),
            connectorClient.connect(info.transport_spec),
        ]), 5000, 'client connect');

        const freePort = await getFreePort();
        const hostPort = `127.0.0.1:${freePort}`;

        rspServer = await withTimeout(
            createServer(listenerClient, info.resource_service_node_id, hostPort, {
                asyncAccept: true,
                childrenAsyncData: true,
            }),
            5000,
            'tcp_listen createServer'
        );

        const acceptedPromise = withTimeout(new Promise((resolve) => {
            rspServer.once('connection', resolve);
        }), 5000, 'tcp_listen accept');

        clientSocket = await withTimeout(
            createConnection(connectorClient, info.resource_service_node_id, hostPort, {
                asyncData: true,
            }),
            5000,
            'tcp_connect to listened socket'
        );

        acceptedSocket = await acceptedPromise;

        const payloadA = Buffer.from('nodejs-listen-minimal-client-to-server', 'utf8');
        clientSocket.write(payloadA);
        const receivedOnServer = await readChunkWithTimeout(acceptedSocket, 5000, 'server receive');
        assert.ok(receivedOnServer.equals(payloadA), 'accepted socket should receive client payload');

        const payloadB = Buffer.from('nodejs-listen-minimal-server-to-client', 'utf8');
        acceptedSocket.write(payloadB);
        const receivedOnClient = await readChunkWithTimeout(clientSocket, 5000, 'client receive');
        assert.ok(receivedOnClient.equals(payloadB), 'client socket should receive server payload');

        console.log('nodejs_tcp_listen_minimal passed');
    } finally {
        if (clientSocket) clientSocket.destroy();
        if (acceptedSocket) acceptedSocket.destroy();
        if (rspServer) await rspServer.close().catch(() => {});
        await listenerClient.close().catch(() => {});
        await connectorClient.close().catch(() => {});
        await terminateProcess(fixture);
    }
}

main().catch((error) => {
    console.error(`nodejs_tcp_listen_minimal failed: ${error.message}`);
    process.exit(1);
});
