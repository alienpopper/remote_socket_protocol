# Python HTTP Server over RSP Integration

This integration runs a Python HTTP server over the RSP socket layer and is
validated by the repository integration harness.

## What is implemented

- `working/app.py`
  - Python HTTP server that uses `client/python/rsp_client.py` and
    `client/python/rsp_net.py` to listen via RSP.
- `test/python_http_server_integration.js`
  - End-to-end harness that starts fixture services and validates HTTP responses
    through RM/RS.
- `Makefile` target `test-python-http-server`
  - Canonical command for this integration path.

## Directory contents

- `modification/example/app.py`
  - Reference copy of the integrated Python app.
- `modification/fetch_and_apply.sh`
  - Placeholder fetch/apply script (packaging workflow not finalized yet).
- `modification/patches/`
  - Placeholder patch files for future reproducible packaging flow.

## Validation

From repository root:

```bash
make test-python-http-server
```

Expected result: `python_http_server_integration passed` and clean teardown.
