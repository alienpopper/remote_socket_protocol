#!/usr/bin/env node
// test_multi_bsd_sockets.js
// Launches RM + 2 bsd_sockets instances, subscribes to logs, and prints events.
// Usage: node scripts/test_multi_bsd_sockets.js

'use strict';
const { spawn } = require('child_process');
const path = require('path');
const { RSPClient, decodeNodeIdField } = require('../client/nodejs/rsp_client');

const REPO = path.join(__dirname, '..');
const BIN = path.join(REPO, 'build', 'bin');

function launchProc(exe, args, label) {
  const proc = spawn(exe, args, { cwd: REPO, stdio: ['ignore', 'pipe', 'pipe'] });
  proc.stdout.on('data', d => process.stdout.write(`[${label}] ${d}`));
  proc.stderr.on('data', d => process.stderr.write(`[${label}] ${d}`));
  proc.on('exit', code => console.log(`[${label}] exited: ${code}`));
  return proc;
}

async function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function main() {
  console.log('Starting RM...');
  const rm = launchProc(path.join(REPO, 'bin', 'esp_rm.exe'), ['--config', path.join(REPO, 'bin', 'rsp_rm.conf')], 'RM');
  await sleep(1500);

  console.log('Starting bsd_sockets #1...');
  const bs1 = launchProc(path.join(BIN, 'rsp_bsd_sockets.exe'),
    ['--config', path.join(REPO, 'bin', 'rsp_bsd_sockets.conf')], 'BSD1');
  await sleep(1000);

  console.log('Starting bsd_sockets #2...');
  const bs2 = launchProc(path.join(BIN, 'rsp_bsd_sockets.exe'),
    ['--config', path.join(REPO, 'bin', 'rsp_bsd_sockets2.conf')], 'BSD2');
  await sleep(2000);

  console.log('\n--- Discovering bsd_sockets nodes via RSP ---');
  const client = new RSPClient();
  await client.connect('tcp:127.0.0.1:3939');
  const rmNodeId = client.peerNodeId;
  console.log('RM node:', rmNodeId);

  const reply = await client.resourceList(rmNodeId, '', 200);
  const services = (reply && reply.services) ? reply.services : [];

  // Filter bsd_sockets nodes (deduplicated by primary schema; each node
  // advertises both bsd_sockets.proto and a logging schema).
  const bsdNodes = [...new Set(
    services
      .filter(s => s.schema && s.schema.proto_file_name === 'bsd_sockets.proto')
      .map(s => decodeNodeIdField(s.node_id.value))
  )];

  console.log('Found bsd_sockets nodes:', bsdNodes);

  if (bsdNodes.length < 2) {
    console.warn('WARNING: expected 2 bsd_sockets nodes, got', bsdNodes.length);
  }

  await client.close().catch(() => {});

  console.log('\nEnvironment ready. Press Ctrl+C to stop.');
  console.log('You can now:');
  console.log('  1. Launch Chrome: C:\\chromium\\src\\out\\Default\\chrome.exe --no-sandbox');
  console.log('  2. Open an RSP window via right-click -> "Open in RSP tab"');
  console.log('  3. Click the router icon, enter RM addr, click Refresh');
  console.log('  4. Two bsd_sockets nodes should appear in the dropdown');
  bsdNodes.forEach((n, i) => console.log(`  bsd_sockets #${i+1}: ${n}`));

  process.on('SIGINT', () => {
    console.log('\nShutting down...');
    rm.kill(); bs1.kill(); bs2.kill();
    process.exit(0);
  });

  // Keep alive
  await new Promise(() => {});
}

main().catch(e => { console.error(e); process.exit(1); });
