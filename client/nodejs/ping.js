'use strict';

const {RSPJsonClient, decodeNodeIdField} = require('./rsp_client');

async function main() {
    const [, , transportSpec, destinationNodeId] = process.argv;
    if (!transportSpec || !destinationNodeId) {
        console.error('Usage: node client/nodejs/ping.js tcp:<host>:<port> <destination-node-id>');
        process.exit(1);
    }

    const client = new RSPJsonClient();
    try {
        await client.connect(transportSpec);
        const reply = await client.ping(destinationNodeId);
        console.log(JSON.stringify({
            local_node_id: client.nodeId,
            resource_manager_node_id: client.peerNodeId,
            reply_destination_node_id: reply.destination ? decodeNodeIdField(reply.destination.value) : null,
            sequence: reply.ping_reply.sequence,
            nonce: reply.ping_reply.nonce.value,
        }, null, 2));
    } finally {
        await client.close().catch(() => {});
    }
}

main().catch((error) => {
    console.error(`nodejs ping failed: ${error.message}`);
    process.exit(1);
});