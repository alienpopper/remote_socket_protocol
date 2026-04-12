# Node.js Express over RSP Integration

This directory documents and packages the Express integration used by the repository test harness.

## What this integration does

The integration runs a regular Express HTTP app over the RSP socket layer by:

1. Connecting a Node.js RSP client to RM (`RSP_TRANSPORT`).
2. Requesting required endorsements from ES.
3. Opening an RSP listen socket on RS (`RSP_RESOURCE_SERVICE_NODE_ID`, `RSP_HOST_PORT`).
4. Adapting each RSP socket to a Node-compatible shape for `http.Server`.
5. Forwarding those sockets into Express by emitting the HTTP server `connection` event.

## Directory contents

- `patches/0001-rsp-app.patch`
  - Converts a baseline Express hello-world app to RSP-aware startup and socket bridge logic.
- `patches/0002-rsp-package.patch`
  - Updates package metadata/scripts used for this integration workflow.
- `example/app.js`
  - Standalone reference app showing the final integrated implementation.
- `fetch_and_apply.sh`
  - Downloads Express source, prepares `integration/nodejs_express/working`, and applies patch files.

## How to rebuild the working app from scratch

From repository root:

```bash
bash integration/nodejs_express/modification/fetch_and_apply.sh
```

This script will:

1. Download Express source archive (default tag: `5.1.0`).
2. Copy the `examples/hello-world` files into `integration/nodejs_express/working`.
3. Normalize baseline `app.js` and `package.json` to deterministic base files.
4. Apply all patches in `modification/patches`.
5. Run `npm install` in `integration/nodejs_express/working`.

## Environment variables used by the integrated app

- `RSP_TRANSPORT` (required for RSP mode)
- `RSP_RESOURCE_SERVICE_NODE_ID` (required for RSP mode)
- `RSP_ENDORSEMENT_NODE_ID` (optional but used in integration test)
- `RSP_HOST_PORT` (optional, defaults to `127.0.0.1:8080`)
- `PORT` (used only in fallback direct TCP mode)

## Validation command

```bash
make test-nodejs-express
```

Expected result: `nodejs_express_integration passed` and clean process exit.
