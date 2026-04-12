'use strict';

// rsp_sshd.js - OpenSSH server forwarder over RSP
//
// Connects to an RSP resource manager as a resource service, listens for
// incoming RSP connections, and for each connection spawns `sshd -i` in
// inetd mode, bridging the RSP socket to sshd's stdin/stdout.
//
// Compatible with systemd: logs to stderr (captured by journald), handles
// SIGTERM cleanly.
//
// Usage:
//   node rsp_sshd.js [/path/to/rsp_sshd.conf.json]
//
// Default config path: /etc/rsp-sshd/rsp_sshd.conf.json

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const { RSPClient } = require('../../../../client/nodejs/rsp_client');
const { createServer: createRSPServer } = require('../../../../client/nodejs/rsp_net');

const ENDORSEMENT_SUCCESS = 0;
const ETYPE_ACCESS = 'f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b';
const EVALUE_ACCESS_NETWORK = 'f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b';
const ETYPE_ROLE = '0963c0ab-215f-42c1-b042-747bf21e330e';
const EVALUE_ROLE_RESOURCE_SERVICE = 'a7f8c9d6-3b2e-4f1a-8c9d-5e6f7a8b9c0d';

function log(message) {
    process.stderr.write(`[rsp-sshd] ${message}\n`);
}

function loadConfig(configPath) {
    const resolved = path.resolve(configPath);
    if (!fs.existsSync(resolved)) {
        throw new Error(`Config file not found: ${resolved}`);
    }
    return JSON.parse(fs.readFileSync(resolved, 'utf8'));
}

async function requestEndorsement(client, nodeId, etype, evalue, label) {
    let reply = null;
    for (let attempt = 0; attempt < 3 && (!reply || reply.status !== ENDORSEMENT_SUCCESS); attempt += 1) {
        reply = await client.beginEndorsementRequest(nodeId, etype, evalue);
    }
    if (!reply || reply.status !== ENDORSEMENT_SUCCESS) {
        throw new Error(`${label} endorsement failed (status=${reply ? reply.status : 'null'})`);
    }
}

// Bridge one RSP socket to a freshly-spawned `sshd -i` process.
// sshd -i (inetd mode) reads the SSH protocol from stdin and writes to stdout.
function handleConnection(socket, config) {
    const sshdBin = config.sshd_path || '/usr/sbin/sshd';
    const sshdArgs = ['-i'];
    if (config.sshd_config) {
        sshdArgs.push('-f', config.sshd_config);
    }
    if (config.sshd_debug) {
        sshdArgs.push('-d');
    }

    log(`Incoming connection; spawning ${sshdBin} ${sshdArgs.join(' ')}`);

    const sshd = spawn(sshdBin, sshdArgs, {
        stdio: ['pipe', 'pipe', 'inherit'],
    });

    // RSP socket → sshd stdin, sshd stdout → RSP socket
    socket.pipe(sshd.stdin);
    sshd.stdout.pipe(socket);

    sshd.on('error', (err) => {
        log(`sshd process error: ${err.message}`);
        socket.destroy();
    });

    sshd.on('exit', (code, signal) => {
        log(`sshd exited (code=${code} signal=${signal})`);
        // Flush any remaining data before destroying
        if (!socket.destroyed) {
            socket.end();
        }
    });

    socket.on('close', () => {
        if (sshd.exitCode === null && !sshd.killed) {
            sshd.kill('SIGTERM');
        }
    });

    socket.on('error', (err) => {
        log(`socket error: ${err.message}`);
        if (sshd.exitCode === null && !sshd.killed) {
            sshd.kill('SIGTERM');
        }
    });
}

async function main() {
    const configPath = process.argv[2] || '/etc/rsp-sshd/rsp_sshd.conf.json';
    const config = loadConfig(configPath);

    const transportSpec = config.rsp_transport;
    const rsNodeId = config.resource_service_node_id;
    const endorsementNodeId = config.endorsement_node_id;
    const hostPort = config.host_port || '127.0.0.1:22';

    if (!transportSpec) {
        throw new Error('Config missing required field: rsp_transport');
    }
    if (!rsNodeId) {
        throw new Error('Config missing required field: resource_service_node_id');
    }

    const client = new RSPClient();
    await client.connect(transportSpec);
    log(`Connected to RSP transport: ${transportSpec}`);

    if (endorsementNodeId) {
        const reachable = await client.ping(endorsementNodeId, 3000);
        if (!reachable) {
            throw new Error('Endorsement service unreachable');
        }
        await requestEndorsement(
            client, endorsementNodeId,
            ETYPE_ACCESS, EVALUE_ACCESS_NETWORK,
            'network access'
        );
        await requestEndorsement(
            client, endorsementNodeId,
            ETYPE_ROLE, EVALUE_ROLE_RESOURCE_SERVICE,
            'resource service role'
        );
        log('Endorsements acquired');
    }

    const rspServer = await createRSPServer(client, rsNodeId, hostPort, {
        asyncAccept: true,
        childrenAsyncData: true,
    });

    log(`Listening on RSP host_port=${hostPort} via RS node=${rsNodeId}`);

    rspServer.on('connection', (socket) => {
        handleConnection(socket, config);
    });

    rspServer.on('close', () => {
        log('RSP server closed');
        process.exit(0);
    });

    const shutdown = async () => {
        log('Shutting down');
        await rspServer.close().catch(() => {});
        await client.close().catch(() => {});
        process.exit(0);
    };

    process.on('SIGINT', shutdown);
    process.on('SIGTERM', shutdown);
}

main().catch((err) => {
    log(`Fatal: ${err.message}`);
    process.exit(1);
});
