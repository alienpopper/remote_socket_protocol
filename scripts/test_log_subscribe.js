'use strict';
/**
 * test_log_subscribe.js
 *
 * Connects to the RM, subscribes to key log types from the RM and bsd_sockets,
 * opens a TCP connection through bsd_sockets (to 1.1.1.1:80), then streams
 * every log record to stdout for 8 seconds before exiting.
 *
 * Usage (from repo root):
 *   node scripts/test_log_subscribe.js
 */

const {RSPClient, encodeNodeIdForField} = require('../client/nodejs/rsp_client');

const RM_TRANSPORT = 'tcp:127.0.0.1:3939';
const RM_NODE_ID   = '7543cff9-b80a-efec-9e20-9dfbb52589ce';
const BSD_NODE_ID  = '58364453-66c5-1f5a-71b7-641840fafe6a';
const DURATION_MS  = 120_000;

const RM_TYPES = [
    'type.rsp/rsp.proto.NodeConnectedEvent',
    'type.rsp/rsp.proto.NodeDisconnectedEvent',
    'type.rsp/rsp.proto.NodeStartedEvent',
    'type.rsp/rsp.proto.NodeStoppingEvent',
];

const BSD_TYPES = [
    'type.rsp/rsp.proto.BsdSocketsConnectEstablishedLog',
    'type.rsp/rsp.proto.BsdSocketsConnectFailedLog',
    'type.rsp/rsp.proto.BsdSocketsPeerClosedLog',
    'type.rsp/rsp.proto.BsdSocketsSocketClosedLog',
    'type.rsp/rsp.proto.BsdSocketsListenStartedLog',
    'type.rsp/rsp.proto.BsdSocketsListenFailedLog',
    'type.rsp/rsp.proto.NodeStartedEvent',
    'type.rsp/rsp.proto.NodeStoppingEvent',
];

// ---------------------------------------------------------------------------
// Extend RSPClient to capture log_record and log_subscribe_reply messages.
// ---------------------------------------------------------------------------

class LoggingClient extends RSPClient {
    constructor() {
        super();
        this._logRecords = [];
        this._logWaiters = [];
    }

    _dispatchMessage(msg) {
        if (msg.log_record != null) {
            this._deliverLogRecord(msg.log_record);
            return;
        }
        if (msg.log_subscribe_reply != null) {
            const r = msg.log_subscribe_reply;
            const status = r.status ?? '?';
            console.log(`[subscribe reply] status=${status}  msg="${r.message ?? ''}"`);
            return;
        }
        super._dispatchMessage(msg);
    }

    _deliverLogRecord(record) {
        if (this._logWaiters.length > 0) {
            const resolve = this._logWaiters.shift();
            resolve(record);
        } else {
            this._logRecords.push(record);
        }
    }

    nextLogRecord(timeoutMs = 8000) {
        if (this._logRecords.length > 0) {
            return Promise.resolve(this._logRecords.shift());
        }
        return new Promise((resolve) => {
            const timer = setTimeout(() => {
                const idx = this._logWaiters.indexOf(resolve);
                if (idx >= 0) this._logWaiters.splice(idx, 1);
                resolve(null);   // null = timeout
            }, timeoutMs);
            this._logWaiters.push((record) => {
                clearTimeout(timer);
                resolve(record);
            });
        });
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function formatLogRecord(record) {
    const payload = record.payload ?? {};
    const typeUrl = payload['@type'] ?? '<unknown>';
    const shortType = typeUrl.includes('/') ? typeUrl.split('/').pop() : typeUrl;
    const producer = record.producer_node_id?.value ?? '';
    const producerShort = producer ? producer.slice(0, 8) : '?';
    const fields = Object.fromEntries(
        Object.entries(payload).filter(([k]) => k !== '@type')
    );
    return `[LOG] ${shortType}  producer=${producerShort}…  ${JSON.stringify(fields)}`;
}

async function subscribeLogs(client, nodeId, typeUrls) {
    for (const typeUrl of typeUrls) {
        const msg = {
            destination: {value: encodeNodeIdForField(nodeId)},
            log_subscribe_request: {
                payload_type_url: typeUrl,
                filter: {true_value: {}},
                duration_ms: DURATION_MS,
            },
        };
        await client._sendSignedMessage(msg);
        console.log(`  → subscribed  ${typeUrl.split('/').pop()}  on ${nodeId.slice(0, 8)}…`);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

async function main() {
    const client = new LoggingClient();

    console.log(`Connecting to RM at ${RM_TRANSPORT} …`);
    await client.connect(RM_TRANSPORT);
    console.log(`Connected.  My node ID = ${client.nodeId}`);
    console.log(`RM peer    node ID = ${client.peerNodeId}`);
    console.log();

    console.log('Subscribing to RM logs …');
    await subscribeLogs(client, RM_NODE_ID, RM_TYPES);

    console.log('Subscribing to bsd_sockets logs …');
    await subscribeLogs(client, BSD_NODE_ID, BSD_TYPES);

    await new Promise(r => setTimeout(r, 400));   // let replies arrive
    console.log();

    // Drain any records already queued (subscribe replies may have triggered none,
    // but NodeStartedEvent might have been missed since services were already up).

    console.log('Opening TCP connection 1.1.1.1:80 through bsd_sockets …');
    let streamId;
    try {
        const reply = await client.connectTCPEx(BSD_NODE_ID, '1.1.1.1:80', {timeoutMs: 5000});
        if (reply && (reply.error || 0) === 0 && reply.stream_id?.value) {
            streamId = reply.stream_id.value;
        } else {
            console.log('  connectTCPEx reply:', JSON.stringify(reply));
        }
    } catch (err) {
        console.log('  connectTCPEx exception:', err.message);
    }
    if (streamId) {
        console.log(`  TCP connect succeeded, stream=${streamId.slice(0, 8)}…`);
        const req = Buffer.from('HEAD / HTTP/1.0\r\nHost: 1.1.1.1\r\n\r\n');
        await client.sendStreamData(streamId, req);
        await new Promise(r => setTimeout(r, 800));
        await client.streamClose(streamId);
        console.log('  Stream closed.');
    } else {
        console.log('  TCP connect FAILED — check that bsd_sockets is running.');
    }

    console.log('\nWaiting up to 8 s for log records …\n');
    let received = 0;
    const deadline = Date.now() + 8000;
    while (Date.now() < deadline) {
        const remaining = deadline - Date.now();
        const record = await client.nextLogRecord(remaining);
        if (record === null) break;
        console.log(formatLogRecord(record));
        received++;
    }

    console.log(`\nDone.  Received ${received} log record(s).`);
    await client.close();
    process.exit(0);
}

main().catch((err) => {
    console.error('Error:', err);
    process.exit(1);
});
