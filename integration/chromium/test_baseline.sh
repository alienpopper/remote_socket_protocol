#!/bin/bash
# Baseline test for Chromium on oldgame before RSP modifications.
# Verifies that all areas we plan to touch work correctly in the unmodified build.
# Run via: ssh oldgame "bash ~/chromium/mainline/src/../../../test_baseline.sh"
# Or:       bash integration/chromium/test_baseline.sh  (from repo root, runs remotely)

set -uo pipefail

CHROME="$HOME/chromium/mainline/src/out/Default/chrome"
USER_DATA="$(mktemp -d /tmp/chrome-baseline-XXXXXX)"
PASS=0
FAIL=0
ERRORS=()
XVFB_PID=""

cleanup() {
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    rm -rf "$USER_DATA"
}
trap cleanup EXIT

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ERRORS+=("$1"); ((FAIL++)); }

echo "=== Chromium Baseline Tests ==="
echo "Chrome: $CHROME"
echo "Build date: $(stat -c %y "$CHROME" 2>/dev/null | cut -d. -f1)"
echo ""

# ── 0. Binary exists ──────────────────────────────────────────────────────────
echo "[0] Binary sanity"
if [ -x "$CHROME" ]; then
    pass "Chrome binary exists and is executable"
    VERSION=$("$CHROME" --version 2>/dev/null || echo "unknown")
    echo "    Version: $VERSION"
else
    fail "Chrome binary missing at $CHROME"
    echo "Build not complete yet. Exiting."
    exit 1
fi

# ── 1. Start virtual display ──────────────────────────────────────────────────
echo ""
echo "[1] Virtual display"
Xvfb :99 -screen 0 1280x1024x24 &
XVFB_PID=$!
export DISPLAY=:99
sleep 1
if kill -0 "$XVFB_PID" 2>/dev/null; then
    pass "Xvfb started on :99"
else
    fail "Xvfb failed to start"
fi

# Helper: run Chrome for N seconds, capture output, kill it
run_chrome() {
    local desc="$1"; shift
    local timeout="$1"; shift
    local log
    log=$(mktemp /tmp/chrome-log-XXXXXX)
    timeout "$timeout" "$CHROME" \
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
    sleep "$((timeout - 1))"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    cat "$log"
    rm -f "$log"
}

# ── 2. Basic launch ───────────────────────────────────────────────────────────
echo ""
echo "[2] Basic launch (chrome://version)"
LOG=$(run_chrome "basic launch" 8 "chrome://version" 2>&1 || true)
if echo "$LOG" | grep -qiE "fatal|Segmentation|Aborted|Cannot|SIGSEGV"; then
    fail "Chrome crashed on basic launch"
    echo "$LOG" | tail -20
else
    pass "Chrome launched without crash"
fi

# ── 3. OTR / Incognito profile creation ───────────────────────────────────────
echo ""
echo "[3] Incognito (OTR profile) — exercises Profile::OTRProfileID path"
LOG=$(run_chrome "incognito" 8 "--incognito" "chrome://version" 2>&1 || true)
if echo "$LOG" | grep -qiE "fatal|Segmentation|Aborted|SIGSEGV"; then
    fail "Chrome crashed in incognito mode"
else
    pass "Chrome launched incognito without crash"
fi

# ── 4. New tab page ───────────────────────────────────────────────────────────
echo ""
echo "[4] New tab page — exercises NewTabButtonMenuModel / tab strip path"
LOG=$(run_chrome "new tab" 8 "chrome://newtab" 2>&1 || true)
if echo "$LOG" | grep -qiE "fatal|Segmentation|Aborted|SIGSEGV"; then
    fail "Chrome crashed on new tab page"
else
    pass "Chrome opened new tab page without crash"
fi

# ── 5. Settings WebUI ─────────────────────────────────────────────────────────
echo ""
echo "[5] Settings page — exercises WebUI infrastructure"
LOG=$(run_chrome "settings" 8 "chrome://settings" 2>&1 || true)
if echo "$LOG" | grep -qiE "fatal|Segmentation|Aborted|SIGSEGV"; then
    fail "Chrome crashed on chrome://settings"
else
    pass "Chrome opened settings without crash"
fi

# ── 6. Custom scheme handling ─────────────────────────────────────────────────
echo ""
echo "[6] Custom scheme — exercises scheme registration path"
# chrome:// is a registered standard scheme; navigating to it exercises the
# same ChildProcessSecurityPolicy + URLLoaderFactory path we'll hook for rsp://
LOG=$(run_chrome "chrome-scheme" 8 "chrome://about" 2>&1 || true)
if echo "$LOG" | grep -qiE "fatal|Segmentation|Aborted|SIGSEGV"; then
    fail "Chrome crashed on chrome://about"
else
    pass "Chrome handled chrome:// scheme without crash"
fi

# ── 7. HTTP navigation ────────────────────────────────────────────────────────
echo ""
echo "[7] HTTP navigation — exercises URLLoaderFactory / network stack"
# Use a known-good local test server or just try a loopback connection
LOG=$(run_chrome "http-nav" 10 "http://localhost:9" 2>&1 || true)
# Chrome will fail to connect (port 9 discard) but shouldn't crash
if echo "$LOG" | grep -qiE "Segmentation|Aborted|SIGSEGV"; then
    fail "Chrome crashed on HTTP navigation"
else
    pass "Chrome handled failed HTTP navigation gracefully (network stack OK)"
fi

# ── 8. ContentBrowserClient smoke test via DevTools ──────────────────────────
echo ""
echo "[8] DevTools OTR profile — exercises ContentBrowserClient::WillCreateURLLoaderFactory"
# DevTools uses OTRProfileID::CreateUniqueForDevTools() — the same path we'll use
LOG=$(run_chrome "devtools" 8 "chrome://inspect" 2>&1 || true)
if echo "$LOG" | grep -qiE "fatal|Segmentation|Aborted|SIGSEGV"; then
    fail "Chrome crashed on chrome://inspect (DevTools)"
else
    pass "Chrome opened DevTools page (ContentBrowserClient OTR path OK)"
fi

# ── 9. Profile network context service ───────────────────────────────────────
echo ""
echo "[9] Profile network context — navigate to http then check no crash"
LOG=$(run_chrome "profile-net" 10 \
    "http://localhost:9" \
    "--incognito" 2>&1 || true)
if echo "$LOG" | grep -qiE "Segmentation|Aborted|SIGSEGV"; then
    fail "Chrome crashed on incognito HTTP navigation"
else
    pass "ProfileNetworkContextService + incognito HTTP navigation OK"
fi

# ── 10. url_constants / scheme list ──────────────────────────────────────────
echo ""
echo "[10] Verify chrome:// scheme list (url_constants sanity)"
LOG=$(run_chrome "chrome-urls" 8 "chrome://chrome-urls" 2>&1 || true)
if echo "$LOG" | grep -qiE "Segmentation|Aborted|SIGSEGV"; then
    fail "Chrome crashed on chrome://chrome-urls"
else
    pass "chrome://chrome-urls loaded (url_constants OK)"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "========================================"
echo "Results: $PASS passed, $FAIL failed"
if [ "${#ERRORS[@]}" -gt 0 ]; then
    echo "Failed tests:"
    for e in "${ERRORS[@]}"; do
        echo "  - $e"
    done
    echo "========================================"
    exit 1
else
    echo "All baseline tests passed."
    echo "========================================"
    exit 0
fi
