# OpenSSH over RSP Integration

This directory documents and packages the OpenSSH integration used by the repository test harness.

## Directory contents

- `patches/`
  - Patch files applied to a baseline OpenSSH configuration to enable RSP transport.
- `example/`
  - Standalone reference configuration showing the final integrated implementation.
- `fetch_and_apply.sh`
  - Downloads OpenSSH source or configuration, prepares `integration/openssh/working`, and applies patch files.

## How to rebuild the working environment from scratch

From repository root:

```bash
bash integration/openssh/modification/fetch_and_apply.sh
```

## Environment variables used by the integrated app

- `RSP_TRANSPORT` (required for RSP mode)
- `RSP_RESOURCE_SERVICE_NODE_ID` (required for RSP mode)
- `RSP_ENDORSEMENT_NODE_ID` (optional but used in integration test)
- `RSP_HOST_PORT` (optional, defaults to `127.0.0.1:22`)

## Validation command

```bash
make test-openssh
```
