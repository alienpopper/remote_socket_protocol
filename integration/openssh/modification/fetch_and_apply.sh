#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WORKING_DIR="$PROJECT_ROOT/integration/openssh/working"
PATCH_DIR="$SCRIPT_DIR/patches"
TMP_DIR="$(mktemp -d)"

cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

OPENSSH_TAG="${OPENSSH_TAG:-V_10_3_P1}"
ARCHIVE_PATH="$TMP_DIR/openssh-${OPENSSH_TAG}.tar.gz"

echo "[rsp-openssh] Downloading OpenSSH source: tag=${OPENSSH_TAG}"
curl -fsSL "https://github.com/openssh/openssh-portable/archive/refs/tags/${OPENSSH_TAG}.tar.gz" -o "$ARCHIVE_PATH"

echo "[rsp-openssh] Extracting archive"
rm -rf "$WORKING_DIR"
mkdir -p "$WORKING_DIR"
tar -xzf "$ARCHIVE_PATH" --strip-components=1 -C "$WORKING_DIR"

echo "[rsp-openssh] Applying patches"
for patch_file in "$PATCH_DIR"/*.patch; do
  [[ -e "$patch_file" ]] || continue
  echo "[rsp-openssh]   -> $(basename "$patch_file")"
  patch -d "$WORKING_DIR" -p1 < "$patch_file"
done

echo "[rsp-openssh] Done. Working source is ready in: $WORKING_DIR"
