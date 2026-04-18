'use strict';

const path = require('path');
const {RSPClient} = require('./rsp_client');

async function main() {
    const [, , transportSpec, destinationNodeId] = process.argv;
    if (!transportSpec || !destinationNodeId) {
        console.error('Usage: node client/nodejs_full/ping.js tcp:<host>:<port> <destination-node-id>');
        process.exit(1);
    }

    // Locate the repo root so messages.proto can be loaded into the registry for
    // callers that switch to protobuf encoding.  The ping itself uses JSON encoding
    // by default (no registry use required).
    const protoRoot = path.resolve(__dirname, '..', '..');

    const client = new RSPClient(null, {protoRoot});
    try {
        await client.connect(transportSpec);
        const ok = await client.ping(destinationNodeId);
        console.log(JSON.stringify({
            local_node_id: client.nodeId,
            resource_manager_node_id: client.peerNodeId,
            destination_node_id: destinationNodeId,
            success: ok,
        }, null, 2));
        if (!ok) process.exit(1);
    } finally {
        await client.close().catch(() => {});
    }
}

main().catch((error) => {
    console.error(`nodejs_full ping failed: ${error.message}`);
    process.exit(1);
});
