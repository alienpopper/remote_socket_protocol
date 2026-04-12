'use strict';

const assert = require('assert');
const net = require('net');
const readline = require('readline');
const {spawn} = require('child_process');

const {RSPClient} = require('../client/nodejs/rsp_client');

const HARNESS_TIMEOUT_MS = 45000;

function withTimeout(promise, timeoutMs, label) {
    return Promise.race([
        promise,
        new Promise((_, reject) => {
            setTimeout(() => reject(new Error(`${label} timed out after ${timeoutMs}ms`)), timeoutMs);
        }),
    ]);
}

function parseTcpTransportSpec(spec) {
    const value = String(spec);
    if (!value.startsWith('tcp:')) {
        throw new Error(`unsupported transport spec for reconnect test: ${value}`);
    }
    const payload = value.slice(4);
    const sep = payload.lastIndexOf(':');
    if (sep <= 0 || sep + 1 >= payload.length) {
        throw new Error(`invalid tcp transport spec: ${value}`);
    }
    return {
        host: payload.slice(0, sep),
        port: Number.parseInt(payload.slice(sep + 1), 10),
    };
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
                resolve(JSON.parse(line));
            } catch {
            }
        });

        stderrReader.on('line', (line) => {
            stderrLines.push(line);
        });

        fixture.once('error', (error) => reject(error));
        fixture.once('exit', (code, signal) => {
            reject(new Error(
                `fixture exited before ready (code=${code}, signal=${signal})\nstdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        });
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
        }, 1500);

        processHandle.once('exit', () => {
            clearTimeout(timer);
            resolve();
        });
    });
}

class TcpBounceProxy {
    constructor(targetHost, targetPort) {
        this._targetHost = targetHost;
        this._targetPort = targetPort;
        this._server = null;
        this._pairs = new Set();
        this._listenPort = null;
    }

    async start() {
        this._server = net.createServer((clientSocket) => {
            const upstreamSocket = net.createConnection({host: this._targetHost, port: this._targetPort});
            const pair = {clientSocket, upstreamSocket};
            this._pairs.add(pair);

            const cleanupPair = () => {
                this._pairs.delete(pair);
            };

            clientSocket.on('close', cleanupPair);
            upstreamSocket.on('close', cleanupPair);

            clientSocket.on('error', () => {});
            upstreamSocket.on('error', () => {});

            clientSocket.pipe(upstreamSocket);
            upstreamSocket.pipe(clientSocket);
        });

        await new Promise((resolve, reject) => {
            this._server.once('error', reject);
            this._server.listen(0, '127.0.0.1', () => {
                const address = this._server.address();
                if (!address || typeof address === 'string') {
                    reject(new Error('proxy failed to allocate listening port'));
                    return;
                }
                this._listenPort = address.port;
                resolve();
            });
        });
    }

    transportSpec() {
        return `tcp:127.0.0.1:${this._listenPort}`;
    }

    dropAllConnections() {
        for (const pair of this._pairs) {
            pair.clientSocket.end();
            pair.upstreamSocket.end();
            setTimeout(() => {
                if (!pair.clientSocket.destroyed) {
                    pair.clientSocket.destroy();
                }
                if (!pair.upstreamSocket.destroyed) {
                    pair.upstreamSocket.destroy();
                }
            }, 100);
        }
        this._pairs.clear();
    }

    async stop() {
        this.dropAllConnections();
        if (!this._server) {
            return;
        }
        await new Promise((resolve) => {
            this._server.close(() => resolve());
        });
    }
}

async function main() {
    const [, , fixturePath] = process.argv;
    if (!fixturePath) {
        throw new Error('Usage: node test/nodejs_client_reconnect_integration.js <fixture-path>');
    }

    const fixture = spawn(fixturePath, [], {stdio: ['ignore', 'pipe', 'pipe']});
    let proxy = null;
    let client = null;

    try {
        const info = await withTimeout(waitForFixtureReady(fixture), 10000, 'fixture readiness');
        const endpoint = parseTcpTransportSpec(info.transport_spec);

        proxy = new TcpBounceProxy(endpoint.host, endpoint.port);
        await withTimeout(proxy.start(), 3000, 'proxy start');

        client = new RSPClient(undefined, {
            autoReconnect: {
                enabled: true,
                initialDelayMs: 100,
                maxDelayMs: 400,
            },
        });

        client.on('reconnecting', (trigger) => {
            console.error(`[reconnect-test] reconnecting trigger=${trigger}`);
        });
        client.on('reconnected', () => {
            console.error('[reconnect-test] reconnected');
        });
        client.on('reconnect_attempt_failed', (error) => {
            console.error(`[reconnect-test] reconnect attempt failed: ${error.message}`);
        });
        client.on('endorsement_needed', () => {
            console.error('[reconnect-test] endorsement_needed received');
        });

        await withTimeout(client.connect(proxy.transportSpec()), 4000, 'initial connect');

        const initialReachable = await withTimeout(client.ping(info.resource_manager_node_id, 2000), 3000, 'initial ping');
        assert.strictEqual(initialReachable, true, 'initial RM ping should succeed');

        const reconnectedPromise = withTimeout(
            new Promise((resolve) => client.once('reconnected', resolve)),
            8000,
            'reconnected event'
        );

        proxy.dropAllConnections();
        await reconnectedPromise;

        assert.strictEqual(
            String(client.peerNodeId || '').toLowerCase(),
            String(info.resource_manager_node_id).toLowerCase(),
            'peer identity should be RM after reconnect handshake'
        );

        for (let attempt = 0; attempt < 3; attempt += 1) {
            try {
                const rmReachable = await withTimeout(
                    client.ping(info.resource_manager_node_id, 2000),
                    3500,
                    `post-reconnect ping ${attempt + 1}`
                );

                const esReachable = await withTimeout(
                    client.ping(info.endorsement_service_node_id, 2000),
                    3500,
                    `post-reconnect endorsement ping ${attempt + 1}`
                );
                console.error(`[reconnect-test] attempt=${attempt + 1} rm=${rmReachable} es=${esReachable}`);
            } catch {
                console.error(`[reconnect-test] attempt=${attempt + 1} ping threw`);
            }
            await new Promise((resolve) => setTimeout(resolve, 250));
        }

        console.log('nodejs_client_reconnect_integration passed');
    } finally {
        if (client) {
            await client.close().catch(() => {});
        }
        if (proxy) {
            await proxy.stop().catch(() => {});
        }
        await terminateProcess(fixture);
    }
}

const watchdog = setTimeout(() => {
    console.error(`nodejs_client_reconnect_integration failed: harness timed out after ${HARNESS_TIMEOUT_MS}ms`);
    process.exit(1);
}, HARNESS_TIMEOUT_MS);

main()
    .then(() => {
        clearTimeout(watchdog);
    })
    .catch((error) => {
        clearTimeout(watchdog);
        console.error(`nodejs_client_reconnect_integration failed: ${error.message}`);
        process.exit(1);
    });