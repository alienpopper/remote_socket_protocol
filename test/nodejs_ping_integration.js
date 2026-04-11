'use strict';

const readline = require('readline');
const {spawn} = require('child_process');

const {RSPClient} = require('../client/nodejs/rsp_client');

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
                const parsed = JSON.parse(line);
                cleanup();
                resolve({
                    info: parsed,
                    stdoutLines,
                    stderrLines,
                });
            } catch {
            }
        });

        stderrReader.on('line', (line) => {
            stderrLines.push(line);
        });

        fixture.once('error', (error) => {
            cleanup();
            reject(error);
        });

        fixture.once('exit', (code, signal) => {
            cleanup();
            reject(new Error(
                `fixture exited before becoming ready (code=${code}, signal=${signal})\nstdout:\n${stdoutLines.join('\n')}\nstderr:\n${stderrLines.join('\n')}`
            ));
        });
    });
}

async function terminateFixture(fixture) {
    if (fixture.exitCode !== null || fixture.killed) {
        return;
    }

    fixture.kill('SIGTERM');
    await new Promise((resolve) => {
        const timer = setTimeout(() => {
            fixture.kill('SIGKILL');
            resolve();
        }, 5000);

        fixture.once('exit', () => {
            clearTimeout(timer);
            resolve();
        });
    });
}

async function main() {
    const [, , fixturePath] = process.argv;
    if (!fixturePath) {
        throw new Error('Usage: node test/nodejs_ping_integration.js <fixture-path>');
    }

    const fixture = spawn(fixturePath, [], {stdio: ['ignore', 'pipe', 'pipe']});
    const {info} = await waitForFixtureReady(fixture);
    const client = new RSPClient();

    try {
        await client.connect(info.transport_spec);
        await client.ping(info.client_service_node_id);
        await client.ping(info.endorsement_service_node_id);
        console.log('nodejs_ping_integration passed');
    } finally {
        await client.close().catch(() => {});
        await terminateFixture(fixture);
    }
}

main().catch((error) => {
    console.error(`nodejs_ping_integration failed: ${error.message}`);
    process.exit(1);
});