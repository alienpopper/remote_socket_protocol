'use strict';
/**
 * debug_chrome_rsp.js
 *
 * Connects to Chrome via CDP (port 9222) and to the RSP stack simultaneously.
 * Navigates Chrome to a test URL, then reports:
 *   - Chrome net-internals events (proxy resolution, socket errors)
 *   - RSP log records from bsd_sockets (connection attempts)
 *
 * Usage (from repo root):
 *   node scripts/debug_chrome_rsp.js [url]
 * Default URL: http://example.com
 */

const http = require('http');
const {WebSocket} = require('./node_modules/ws/');
const {RSPClient, encodeNodeIdForField} = require('../client/nodejs/rsp_client');

const TARGET_URL  = process.argv[2] || 'http://example.com';
const CDP_HOST    = '127.0.0.1';
const CDP_PORT    = 9222;
const WAIT_MS     = 12000;   // time to collect logs after navigation

const RM_TRANSPORT = 'tcp:127.0.0.1:3939';
const RM_NODE_ID   = '7543cff9-b80a-efec-9e20-9dfbb52589ce';
const BSD_NODE_ID  = '58364453-66c5-1f5a-71b7-641840fafe6a';
const DURATION_MS  = 60_000;

// ---------------------------------------------------------------------------
// CDP helpers
// ---------------------------------------------------------------------------

function cdpGet(path) {
    return new Promise((resolve, reject) => {
        http.get({host: CDP_HOST, port: CDP_PORT, path}, (res) => {
            let body = '';
            res.on('data', d => body += d);
            res.on('end', () => {
                try { resolve(JSON.parse(body)); } catch { resolve(body); }
            });
        }).on('error', reject);
    });
}

class CDPSession {
    constructor(wsUrl) {
        this._ws = new WebSocket(wsUrl);
        this._id = 1;
        this._pending = new Map();
        this._listeners = new Map();
        this._ready = new Promise((resolve, reject) => {
            this._ws.on('open', resolve);
            this._ws.on('error', reject);
        });
        this._ws.on('message', (raw) => {
            const msg = JSON.parse(raw);
            if (msg.id && this._pending.has(msg.id)) {
                const {resolve, reject} = this._pending.get(msg.id);
                this._pending.delete(msg.id);
                if (msg.error) reject(new Error(msg.error.message));
                else resolve(msg.result);
            } else if (msg.method) {
                const handlers = this._listeners.get(msg.method) || [];
                for (const h of handlers) h(msg.params);
            }
        });
    }

    async send(method, params = {}) {
        await this._ready;
        return new Promise((resolve, reject) => {
            const id = this._id++;
            this._pending.set(id, {resolve, reject});
            this._ws.send(JSON.stringify({id, method, params}));
        });
    }

    on(event, handler) {
        const handlers = this._listeners.get(event) || [];
        handlers.push(handler);
        this._listeners.set(event, handlers);
    }

    close() { this._ws.close(); }
}

// ---------------------------------------------------------------------------
// RSP log client
// ---------------------------------------------------------------------------

class LoggingClient extends RSPClient {
    constructor() {
        super();
        this._logRecords = [];
    }

    _dispatchMessage(msg) {
        if (msg.log_record != null) {
            const record = msg.log_record;
            const typeUrl = record.payload?.['@type'] ?? '';
            const shortType = typeUrl.split('/').pop();
            const producer = record.producer_node_id?.value ?? '';
            const producerShort = producer ? producer.slice(0, 8) : '?';
            const fields = Object.fromEntries(
                Object.entries(record.payload ?? {}).filter(([k]) => k !== '@type')
            );
            console.log(`[RSP LOG] ${shortType}  from=${producerShort}…  ${JSON.stringify(fields)}`);
            this._logRecords.push(record);
            return;
        }
        if (msg.log_subscribe_reply != null) return;  // suppress
        super._dispatchMessage(msg);
    }
}

async function subscribeLogs(client, nodeId, typeUrls) {
    for (const typeUrl of typeUrls) {
        await client._sendSignedMessage({
            destination: {value: encodeNodeIdForField(nodeId)},
            log_subscribe_request: {
                payload_type_url: typeUrl,
                filter: {true_value: {}},
                duration_ms: DURATION_MS,
            },
        });
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

async function main() {
    // 1. Connect RSP log client
    console.log('Connecting to RSP stack …');
    const rsp = new LoggingClient();
    await rsp.connect(RM_TRANSPORT);
    console.log(`  RSP connected.  My node ID = ${rsp.nodeId}`);

    await subscribeLogs(rsp, RM_NODE_ID, [
        'type.rsp/rsp.proto.NodeConnectedEvent',
        'type.rsp/rsp.proto.NodeDisconnectedEvent',
    ]);
    await subscribeLogs(rsp, BSD_NODE_ID, [
        'type.rsp/rsp.proto.BsdSocketsConnectEstablishedLog',
        'type.rsp/rsp.proto.BsdSocketsConnectFailedLog',
        'type.rsp/rsp.proto.BsdSocketsPeerClosedLog',
        'type.rsp/rsp.proto.BsdSocketsSocketClosedLog',
    ]);
    console.log('  RSP log subscriptions active.\n');

    // 2. Connect to Chrome via CDP
    console.log('Connecting to Chrome CDP …');
    const tabs = await cdpGet('/json/list');
    let tab = tabs.find(t => t.type === 'page');
    if (!tab) {
        // open a new tab
        const newTab = await cdpGet('/json/new');
        tab = newTab;
    }
    console.log(`  Attaching to tab: ${tab.title || tab.url}`);
    const cdp = new CDPSession(tab.webSocketDebuggerUrl);
    await cdp._ready;

    // Enable Network and Page domains for event capture
    await cdp.send('Network.enable');
    await cdp.send('Page.enable');

    const netEvents = [];
    cdp.on('Network.requestWillBeSent', (p) => {
        console.log(`[CDP] requestWillBeSent  ${p.request.url}`);
        netEvents.push({type: 'requestWillBeSent', ...p});
    });
    cdp.on('Network.responseReceived', (p) => {
        console.log(`[CDP] responseReceived    ${p.response.url}  status=${p.response.status}`);
    });
    cdp.on('Network.loadingFailed', (p) => {
        console.log(`[CDP] loadingFailed       errorText="${p.errorText}"  blocked=${p.blockedReason}`);
    });
    cdp.on('Page.loadEventFired', () => {
        console.log('[CDP] page load event fired');
    });

    // 3. Navigate to test URL
    console.log(`\nNavigating to: ${TARGET_URL}\n`);
    await cdp.send('Page.navigate', {url: TARGET_URL});

    // 4. Wait for logs
    console.log(`Collecting logs for ${WAIT_MS / 1000}s …\n`);
    await new Promise(r => setTimeout(r, WAIT_MS));

    // 5. Grab chrome://net-internals proxy info via JS eval
    console.log('\n--- Checking RSP proxy prefs via JS ---');
    try {
        const prefsResult = await cdp.send('Runtime.evaluate', {
            expression: `JSON.stringify({
                href: window.location.href,
                title: document.title
            })`,
            returnByValue: true,
        });
        console.log('Page state:', prefsResult.result?.value);
    } catch (e) {
        console.log('JS eval failed:', e.message);
    }

    // 6. Summary
    console.log(`\n--- Summary ---`);
    console.log(`RSP log records received: ${rsp._logRecords.length}`);
    console.log(`Chrome net events: ${netEvents.length}`);
    if (rsp._logRecords.length === 0) {
        console.log('\nNO bsd_sockets activity — Chrome is NOT routing through RSP proxy.');
        console.log('Check: chrome://settings/rsp or chrome://net-internals/#proxy');
    }

    cdp.close();
    rsp.socket.destroy();
    process.exit(0);
}

main().catch(err => {
    console.error('Fatal:', err);
    process.exit(1);
});
