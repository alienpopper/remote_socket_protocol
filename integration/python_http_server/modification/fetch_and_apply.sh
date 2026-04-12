#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WORKING_DIR="$PROJECT_ROOT/integration/python_http_server/working"
PATCH_DIR="$SCRIPT_DIR/patches"

echo "[python-http-server] scaffold fetch/apply script"
echo "[python-http-server] working dir: $WORKING_DIR"
echo "[python-http-server] patch dir:   $PATCH_DIR"
echo "[python-http-server] TODO: add source download + patch application logic"
