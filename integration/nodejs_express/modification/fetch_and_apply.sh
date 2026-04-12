#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WORKING_DIR="$PROJECT_ROOT/integration/nodejs_express/working"
PATCH_DIR="$SCRIPT_DIR/patches"
TMP_DIR="$(mktemp -d)"

cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

EXPRESS_TAG="${EXPRESS_TAG:-5.1.0}"
ARCHIVE_PATH="$TMP_DIR/express-${EXPRESS_TAG}.tar.gz"

download_archive() {
  local tag="$1"
  local url="https://github.com/expressjs/express/archive/refs/tags/${tag}.tar.gz"
  echo "[rsp-express] Downloading Express source: $url"
  curl -fsSL "$url" -o "$ARCHIVE_PATH"
}

if ! download_archive "$EXPRESS_TAG"; then
  if [[ "$EXPRESS_TAG" != v* ]]; then
    echo "[rsp-express] Retry with v-prefixed tag"
    download_archive "v${EXPRESS_TAG}"
  else
    echo "[rsp-express] Retry with non-prefixed tag"
    download_archive "${EXPRESS_TAG#v}"
  fi
fi

echo "[rsp-express] Extracting archive"
tar -xzf "$ARCHIVE_PATH" -C "$TMP_DIR"
SRC_DIR="$(find "$TMP_DIR" -maxdepth 1 -type d -name "express-*" | head -n 1)"
if [[ -z "$SRC_DIR" ]]; then
  echo "[rsp-express] Failed to locate extracted Express source directory" >&2
  exit 1
fi

EXAMPLE_DIR="$SRC_DIR/examples/hello-world"
if [[ ! -d "$EXAMPLE_DIR" ]]; then
  echo "[rsp-express] Missing hello-world example in downloaded Express source" >&2
  exit 1
fi

echo "[rsp-express] Preparing working directory: $WORKING_DIR"
rm -rf "$WORKING_DIR"
mkdir -p "$WORKING_DIR"
cp -R "$EXAMPLE_DIR/"* "$WORKING_DIR/"

if [[ -f "$WORKING_DIR/index.js" && ! -f "$WORKING_DIR/app.js" ]]; then
  mv "$WORKING_DIR/index.js" "$WORKING_DIR/app.js"
fi

# Normalize baseline files to a known starting point so patches apply deterministically.
cat > "$WORKING_DIR/app.js" <<'EOF'
const express = require("express");

const app = express();
const port = Number.parseInt(process.env.PORT || "3000", 10);

app.get("/", (_req, res) => {
  res.send("Hello World!");
});

app.listen(port, () => {
  console.log(`Example app listening on port ${port}`);
});
EOF

cat > "$WORKING_DIR/package.json" <<'EOF'
{
  "name": "working",
  "version": "1.0.0",
  "private": true,
  "main": "app.js",
  "scripts": {
    "start": "node app.js"
  },
  "dependencies": {
    "express": "^5.1.0"
  }
}
EOF

echo "[rsp-express] Applying patches"
for patch_file in "$PATCH_DIR"/*.patch; do
  echo "[rsp-express]   -> $(basename "$patch_file")"
  patch -d "$WORKING_DIR" -p1 < "$patch_file"
done

echo "[rsp-express] Installing npm dependencies"
(
  cd "$WORKING_DIR"
  npm install
)

echo "[rsp-express] Done. Working app is ready in: $WORKING_DIR"
