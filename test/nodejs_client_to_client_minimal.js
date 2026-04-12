'use strict';

const readline = require('readline');
const {spawn} = require('child_process');

const {RSPClient} = require('../client/nodejs/rsp_client');

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
                // wait for JSON readiness line
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
        throw new Error('Usage: node test/nodejs_client_to_client_minimal.js <fixture-path>');
    }

    const fixture = spawn(fixturePath, [], {stdio: ['ignore', 'pipe', 'pipe']});
    const clientA = new RSPClient();
    const clientB = new RSPClient();

    try {
        const {info} = await withTimeout(waitForFixtureReady(fixture), 5000, 'fixture readiness');

        await withTimeout(Promise.all([
            clientA.connect(info.transport_spec),
            clientB.connect(info.transport_spec),
        ]), 5000, 'client connect');

        for (let i = 0; i < 100; i += 1) {
            const ok = await withTimeout(clientA.ping(clientB.nodeId, 1000), 1500, `ping #${i + 1}`);
            if (!ok) {
                throw new Error(`client-to-client ping failed at iteration ${i + 1}`);
            }
        }

        console.log('nodejs_client_to_client_minimal passed (100/100 pings)');
    } finally {
        await clientA.close().catch(() => {});
        await clientB.close().catch(() => {});
        await terminateProcess(fixture);
    }
}

main().catch((error) => {
    console.error(`nodejs_client_to_client_minimal failed: ${error.message}`);
    process.exit(1);
});
