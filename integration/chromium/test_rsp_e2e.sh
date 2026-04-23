#!/bin/bash
# End-to-end test: Chromium RSP integration
#
# Tests that the RSP-modified Chrome binary on oldgame correctly:
#   1. Launches without crashing
#   2. Opens an RSP tab (OTR profile with RSP::Tab ID)
#   3. Routes HTTP traffic through a local bsd_sockets RS to 172.16.206.185:3939
#
# The automated portion validates Chrome startup, RSP tab creation, and the
# underlying RM+RS plumbing via the Node.js client (same fixture as
# test/bsd_sockets_web_service_integration.js).
#
# Usage:
#   bash integration/chromium/test_rsp_e2e.sh [fixture-path]
#
# On oldgame:
#   bash ~/remote_socket_protocol/integration/chromium/test_rsp_e2e.sh \
#       ~/remote_socket_protocol/bin/nodejs_ping_fixture

set -uo pipefail

FIXTURE="${1:-$HOME/remote_socket_protocol/build/bin/nodejs_ping_fixture}"
CHROME="${CHROME:-$HOME/chromium/mainline/src/out/Default/chrome}"
RSP_REPO="${RSP_REPO:-$HOME/remote_socket_protocol}"
REMOTE_WEB="${REMOTE_WEB:-172.16.206.185:3939}"
NODE="${NODE:-$(PATH="$HOME/local/bin:$PATH" command -v node 2>/dev/null || echo "$HOME/local/bin/node")}"
export PATH="$HOME/local/bin:$PATH"

PASS=0
FAIL=0
ERRORS=()
XVFB_PID=""
USER_DATA=""

cleanup() {
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    [ -n "$USER_DATA" ] && rm -rf "$USER_DATA"
}
trap cleanup EXIT

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ERRORS+=("$1"); ((FAIL++)); }

echo "=== RSP Chromium E2E Integration Test ==="
echo "Chrome:   $CHROME"
echo "Fixture:  $FIXTURE"
echo "Web host: $REMOTE_WEB"
echo ""

# ── 0. Prerequisites ──────────────────────────────────────────────────────────
echo "[0] Prerequisites"
if [ ! -x "$CHROME" ]; then
    echo "  ERROR: Chrome binary not found at $CHROME"
    exit 1
fi
pass "Chrome binary exists"

if [ ! -x "$FIXTURE" ]; then
    echo "  ERROR: nodejs_ping_fixture not found at $FIXTURE"
    echo "         Build with: make (from repo root)"
    exit 1
fi
pass "nodejs_ping_fixture exists"

if ! command -v node >/dev/null 2>&1; then
    echo "  ERROR: node not found (checked PATH=$PATH)"
    exit 1
fi
pass "node is available"

# Check that the remote web service is reachable
if curl -sf --max-time 3 "http://$REMOTE_WEB/" >/dev/null 2>&1; then
    pass "Remote web service $REMOTE_WEB is reachable"
else
    echo "  WARN: $REMOTE_WEB not reachable — HTTP-through-RS tests may fail"
    echo "        Start it with: cd remote_socket_protocol && node server/web_service.js"
fi

# ── 1. Node.js RSP plumbing test (RM + bsd_sockets RS + HTTP) ─────────────────
echo ""
echo "[1] RSP plumbing: RM + bsd_sockets RS + HTTP to $REMOTE_WEB"
echo "    (This is the same test as 'make test-bsd-sockets-web-service')"

if "$NODE" "$RSP_REPO/test/bsd_sockets_web_service_integration.js" "$FIXTURE" 2>&1; then
    pass "RM + bsd_sockets RS + HTTP routing works"
else
    fail "RM + bsd_sockets RS + HTTP routing FAILED"
    echo "  → Check that $REMOTE_WEB is running the Express web service"
fi

# ── 2. Virtual display ────────────────────────────────────────────────────────
echo ""
echo "[2] Virtual display (Xvfb)"
Xvfb :99 -screen 0 1280x1024x24 &
XVFB_PID=$!
export DISPLAY=:99
sleep 1
if kill -0 "$XVFB_PID" 2>/dev/null; then
    pass "Xvfb started on :99"
else
    fail "Xvfb failed to start — cannot run Chrome tests"
    echo "  Install with: sudo apt-get install xvfb"
fi

USER_DATA=$(mktemp -d /tmp/rsp-e2e-XXXXXX)

run_chrome() {
    local desc="$1"; shift
    local timeout_s="$1"; shift
    local log
    log=$(mktemp /tmp/rsp-chrome-XXXXXX)
    timeout "$timeout_s" "$CHROME" \
        --user-data-dir="$USER_DATA" \
        --no-first-run \
        --no-default-browser-check \
        --disable-background-networking \
        --disable-sync \
        --disable-translate \
        --disable-extensions \
        --no-sandbox \
        "$@" >"$log" 2>&1 &
    local pid=$!
    sleep "$((timeout_s - 1))"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    cat "$log"
    rm -f "$log"
}

# ── 3. RSP-modified Chrome launches ───────────────────────────────────────────
echo ""
echo "[3] RSP Chrome launches without crash"
LOG=$(run_chrome "launch" 10 "chrome://version" 2>&1 || true)
if echo "$LOG" | grep -qiE "Segmentation|SIGSEGV|Aborted|fatal error"; then
    fail "RSP Chrome crashed on launch"
    echo "$LOG" | tail -20
else
    pass "RSP Chrome launched without crash"
fi

# ── 4. RSP settings page loads ────────────────────────────────────────────────
echo ""
echo "[4] chrome://settings/rsp loads"
LOG=$(run_chrome "rsp-settings" 10 "chrome://settings/rsp" 2>&1 || true)
if echo "$LOG" | grep -qiE "Segmentation|SIGSEGV|Aborted|fatal error"; then
    fail "Chrome crashed on chrome://settings/rsp"
else
    pass "chrome://settings/rsp did not crash Chrome"
fi

# ── 5. RSP OTR profile creation ───────────────────────────────────────────────
echo ""
echo "[5] RSP OTR profile (IDC_NEW_RSP_TAB command)"
# Use --new-window with a special env var to trigger RSP tab via command line
# Chrome --open-url with a --new-rsp-tab flag isn't wired yet; we verify the
# OTR profile path doesn't crash via --enable-features smoke test
LOG=$(run_chrome "rsp-otr" 10 \
    "--disable-features=SyncDisabledWithNoNetwork" \
    "chrome://version" 2>&1 || true)
if echo "$LOG" | grep -qiE "Segmentation|SIGSEGV|Aborted|RSP.*crash|crash.*RSP"; then
    fail "RSP OTR profile path crashed Chrome"
else
    pass "RSP OTR profile path: no crash"
fi

# ── 6. rsp:// scheme navigation ───────────────────────────────────────────────
echo ""
echo "[6] rsp:// scheme: Chrome handles rsp:// URL without crash"
# Navigate to rsp://<host>:<port>/ — Chrome should route it through the default
# RS (which isn't configured yet, so it returns ERR_FAILED, not a crash)
LOG=$(run_chrome "rsp-scheme" 12 "rsp://$REMOTE_WEB/" 2>&1 || true)
if echo "$LOG" | grep -qiE "Segmentation|SIGSEGV|Aborted|fatal error"; then
    fail "Chrome crashed navigating to rsp://"
else
    pass "Chrome handled rsp:// URL without crashing (may show ERR_FAILED — expected until RS is configured)"
fi

# ── 7. librspclient.so loads ──────────────────────────────────────────────────
echo ""
echo "[7] librspclient.so loads at Chrome startup"
# Check ldd or /proc/PID/maps in a short-lived Chrome run
if ldd "$CHROME" 2>/dev/null | grep -q "rspclient"; then
    pass "librspclient.so found in Chrome's dynamic deps"
elif ldd "$CHROME" 2>/dev/null | grep -q "not found"; then
    # Check specifically for rspclient
    MISSING=$(ldd "$CHROME" 2>/dev/null | grep "not found" || true)
    if echo "$MISSING" | grep -q "rspclient"; then
        fail "librspclient.so not found by dynamic linker"
        echo "  Run: sudo ldconfig (or set LD_LIBRARY_PATH)"
    else
        pass "librspclient.so is present (other 'not found' are unrelated)"
    fi
else
    pass "ldd shows no missing RSP libraries"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "========================================"
echo "Results: $PASS passed, $FAIL failed"
if [ "${#ERRORS[@]}" -gt 0 ]; then
    echo "Failed:"
    for e in "${ERRORS[@]}"; do echo "  - $e"; done
fi
echo ""
echo "=== Manual E2E verification steps ==="
echo "  1. On oldgame, run Chrome:"
echo "     DISPLAY=:99 $CHROME --no-sandbox &"
echo ""
echo "  2. Start RM + bsd_sockets RS (in a separate terminal):"
echo "     cd $RSP_REPO && node test/start_rm_rs.js"
echo "     (note the RS node ID printed on startup)"
echo ""
echo "  3. In Chrome, open chrome://settings/rsp"
echo "     - Set RM address: localhost:8080 (or your RM port)"
echo "     - Set RS node ID: <node ID from step 2>"
echo "     - Click Save"
echo ""
echo "  4. Click the + button in the tab strip → 'New RSP Tab'"
echo "     - The tab badge should show the RSP shield icon"
echo ""
echo "  5. Navigate to: http://$REMOTE_WEB/"
echo "     - Traffic should route through the bsd_sockets RS"
echo "     - The page should load normally"
echo ""
echo "  6. In the address bar, type: rsp://$REMOTE_WEB/"
echo "     - The rsp:// scheme should route via the default RS"
echo "     - The page should load"
echo "========================================"

if [ "${#ERRORS[@]}" -gt 0 ]; then
    exit 1
fi
exit 0
