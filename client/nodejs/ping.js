'use strict';

const {RSPClient} = require('./rsp_client');

async function main() {
    const [, , transportSpec, destinationNodeId] = process.argv;
    if (!transportSpec || !destinationNodeId) {
        console.error('Usage: node client/nodejs/ping.js tcp:<host>:<port> <destination-node-id>');
        process.exit(1);
    }

    const client = new RSPClient();
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
    console.error(`nodejs ping failed: ${error.message}`);
    process.exit(1);
});
