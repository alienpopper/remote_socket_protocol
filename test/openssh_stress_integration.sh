#!/usr/bin/env bash
# openssh_stress_integration.sh — stress tests for SSH over RSP
#
# Starts its own RM, ES, and rsp_sshd, then exercises them with:
#   1. Concurrent SSH command sessions (10 parallel)
#   2. Large file SCP round-trip with checksum verification (10 MB)
#   3. Rapid connect/disconnect (20 sequential quick connections)
#   4. Concurrent SCP uploads (5 parallel × 2 MB)
#   5. High-throughput pipe (50 MB zeros through ssh cat)
#
# Usage:
#   bash test/openssh_stress_integration.sh [BIN_DIR]
#
# BIN_DIR defaults to ./build/bin (relative to repo root).
# Run from the repo root, or from any directory by passing BIN_DIR.
#
# Requirements (all standard on Ubuntu 20.04):
#   sshd, ssh, scp, ssh-keygen, sha256sum, /usr/lib/openssh/sftp-server

set -euo pipefail

# ── paths ────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="${1:-$REPO_ROOT/build/bin}"

RM_BIN="$BIN_DIR/resource_manager"
ES_BIN="$BIN_DIR/endorsement_service"
SSHD_BIN="$BIN_DIR/rsp_sshd"
RSP_SSH_BIN="$BIN_DIR/rsp_ssh"

for bin in "$RM_BIN" "$ES_BIN" "$SSHD_BIN" "$RSP_SSH_BIN"; do
    [[ -x "$bin" ]] || { echo "FAIL: not executable: $bin"; exit 1; }
done

# ── workspace ────────────────────────────────────────────────────────────────
WORK="$(mktemp -d /tmp/rsp_ssh_stress.XXXXXX)"
RM_LOG="$WORK/rm.log"
ES_LOG="$WORK/es.log"
SSHD_LOG="$WORK/rsp_sshd.log"
HOST_KEY_DIR="$WORK/host-keys"
CLIENT_KEY="$WORK/id_test"
AUTHORIZED_KEYS="$WORK/authorized_keys"
SSHD_CONFIG="$WORK/sshd_config"
RSP_SSH_CONF="$WORK/rsp_ssh.conf.json"
LARGE_FILE_LOCAL="$WORK/large_local.bin"
LARGE_FILE_RECV="$WORK/large_recv.bin"
REMOTE_TEMP_DIR="$WORK/remote"

mkdir -p "$HOST_KEY_DIR" "$REMOTE_TEMP_DIR"

RM_PID="" ES_PID="" SSHD_PID=""
PASS=0 FAIL=0
TEST_START_S=$SECONDS

# ── helpers ──────────────────────────────────────────────────────────────────
log()  { echo "[stress][+$((SECONDS - TEST_START_S))s] $*"; }
pass() { PASS=$((PASS + 1)); log "PASS  $*"; }
fail() { FAIL=$((FAIL + 1)); log "FAIL  $*"; }

wait_for_file_line() {
    local file="$1" pattern="$2" timeout_s="${3:-15}"
    local deadline=$((SECONDS + timeout_s))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -qE "$pattern" "$file" 2>/dev/null; then return 0; fi
        sleep 0.2
    done
    return 1
}

extract_line() {
    grep -oE "$2" "$1" | head -1
}

cleanup() {
    log "cleanup"
    [[ -n "$SSHD_PID" ]] && kill "$SSHD_PID" 2>/dev/null || true
    [[ -n "$ES_PID"   ]] && kill "$ES_PID"   2>/dev/null || true
    [[ -n "$RM_PID"   ]] && kill "$RM_PID"   2>/dev/null || true
    wait "$SSHD_PID" "$ES_PID" "$RM_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# ── pick a free port ─────────────────────────────────────────────────────────
free_port() {
    python3 -c "import socket; s=socket.socket(); s.bind(('',0)); p=s.getsockname()[1]; s.close(); print(p)"
}

# ── start RM ─────────────────────────────────────────────────────────────────
RM_PORT=$(free_port)
RM_ENDPOINT="127.0.0.1:$RM_PORT"
log "Starting RM on $RM_ENDPOINT"
"$RM_BIN" "$RM_ENDPOINT" >"$RM_LOG" 2>&1 &
RM_PID=$!
wait_for_file_line "$RM_LOG" "listening on" 10 || { echo "FAIL: RM did not start"; exit 1; }
log "RM up (pid=$RM_PID)"

# ── start ES ─────────────────────────────────────────────────────────────────
log "Starting ES"
"$ES_BIN" "tcp:$RM_ENDPOINT" >"$ES_LOG" 2>&1 &
ES_PID=$!
wait_for_file_line "$ES_LOG" "node ID:" 15 || { echo "FAIL: ES did not emit node ID"; exit 1; }
ES_NODE_ID=$(grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' "$ES_LOG" | head -1)
[[ -n "$ES_NODE_ID" ]] || { echo "FAIL: could not parse ES node ID"; exit 1; }
log "ES up (pid=$ES_PID, node=$ES_NODE_ID)"

# ── generate host + client SSH keys ──────────────────────────────────────────
ssh-keygen -t ed25519 -f "$HOST_KEY_DIR/ssh_host_ed25519_key" -N "" -q
ssh-keygen -t rsa     -b 2048 -f "$HOST_KEY_DIR/ssh_host_rsa_key"     -N "" -q
ssh-keygen -t ed25519 -f "$CLIENT_KEY" -N "" -q
cat "${CLIENT_KEY}.pub" > "$AUTHORIZED_KEYS"
chmod 600 "$AUTHORIZED_KEYS" "$CLIENT_KEY"

# ── minimal sshd_config ───────────────────────────────────────────────────────
SFTP_SERVER=""
for p in /usr/lib/openssh/sftp-server /usr/libexec/sftp-server /usr/lib/ssh/sftp-server; do
    [[ -x "$p" ]] && { SFTP_SERVER="$p"; break; }
done
[[ -n "$SFTP_SERVER" ]] || { echo "FAIL: sftp-server not found (needed for SCP)"; exit 1; }

cat >"$SSHD_CONFIG" <<EOF
HostKey $HOST_KEY_DIR/ssh_host_ed25519_key
HostKey $HOST_KEY_DIR/ssh_host_rsa_key
AuthorizedKeysFile $AUTHORIZED_KEYS
StrictModes no
PubkeyAuthentication yes
PasswordAuthentication no
ChallengeResponseAuthentication no
UsePAM no
PrintMotd no
Subsystem sftp $SFTP_SERVER
EOF

# ── rsp_sshd config ───────────────────────────────────────────────────────────
RSPD_CONF="$WORK/rsp_sshd.conf.json"
cat >"$RSPD_CONF" <<EOF
{
  "rsp_transport": "tcp:$RM_ENDPOINT",
  "sshd_path": "/usr/sbin/sshd",
  "sshd_config": "$SSHD_CONFIG",
  "sshd_debug": false
}
EOF

# ── start rsp_sshd ────────────────────────────────────────────────────────────
log "Starting rsp_sshd"
"$SSHD_BIN" "$RSPD_CONF" >"$SSHD_LOG" 2>&1 &
SSHD_PID=$!
wait_for_file_line "$SSHD_LOG" "Node ID:" 20 || { echo "FAIL: rsp_sshd did not emit Node ID"; cat "$SSHD_LOG"; exit 1; }
SSHD_NODE_ID=$(grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' "$SSHD_LOG" | head -1)
[[ -n "$SSHD_NODE_ID" ]] || { echo "FAIL: could not parse rsp_sshd node ID"; exit 1; }
log "rsp_sshd up (pid=$SSHD_PID, node=$SSHD_NODE_ID)"

# ── rsp_ssh client config ─────────────────────────────────────────────────────
cat >"$RSP_SSH_CONF" <<EOF
{
  "rsp_transport": "tcp:$RM_ENDPOINT",
  "resource_service_node_id": "$SSHD_NODE_ID",
  "endorsement_node_id": "$ES_NODE_ID",
  "host_port": "127.0.0.1:22",
  "connect_timeout_ms": 8000,
  "connect_retries": 2
}
EOF

SSH_OPTS=(
    -i "$CLIENT_KEY"
    -o "ProxyCommand=$RSP_SSH_BIN $RSP_SSH_CONF"
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o BatchMode=yes
    -o ConnectTimeout=15
    -o LogLevel=ERROR
)
SSH_TARGET="$(whoami)@rsp-host"

# ── verify baseline ───────────────────────────────────────────────────────────
log "Baseline connection check"
if output=$(ssh "${SSH_OPTS[@]}" "$SSH_TARGET" 'echo rsp-ok' 2>/dev/null) && [[ "$output" == "rsp-ok" ]]; then
    pass "baseline SSH"
else
    echo "FAIL: baseline SSH failed — output: '$output'"
    echo "--- rsp_sshd log ---"
    cat "$SSHD_LOG"
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 1: Concurrent SSH sessions (10 parallel)
# ─────────────────────────────────────────────────────────────────────────────
log "TEST 1: 10 concurrent SSH command sessions"
CONCURRENT=10
declare -a PIDS TMPOUTS
for i in $(seq 0 $((CONCURRENT - 1))); do
    TMPOUTS[$i]="$WORK/sess_$i.out"
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "echo session-$i" >"${TMPOUTS[$i]}" 2>/dev/null &
    PIDS[$i]=$!
done

T1_FAILURES=0
for i in $(seq 0 $((CONCURRENT - 1))); do
    if wait "${PIDS[$i]}" 2>/dev/null; then
        got=$(cat "${TMPOUTS[$i]}" 2>/dev/null)
        if [[ "$got" != "session-$i" ]]; then
            T1_FAILURES=$((T1_FAILURES + 1))
            log "  session-$i: wrong output: '$got'"
        fi
    else
        T1_FAILURES=$((T1_FAILURES + 1))
        log "  session-$i: ssh exited non-zero"
    fi
done
if [[ $T1_FAILURES -eq 0 ]]; then
    pass "10 concurrent sessions (all correct output)"
else
    fail "10 concurrent sessions ($T1_FAILURES/$CONCURRENT failed)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 2: Large file SCP round-trip (10 MB)
# ─────────────────────────────────────────────────────────────────────────────
log "TEST 2: Large file SCP round-trip (10 MB)"
LARGE_SIZE_MB=10
dd if=/dev/urandom of="$LARGE_FILE_LOCAL" bs=1M count=$LARGE_SIZE_MB 2>/dev/null
LOCAL_SHA=$(sha256sum "$LARGE_FILE_LOCAL" | awk '{print $1}')

REMOTE_LARGE="/tmp/rsp_stress_large_$$.bin"
SCP_OPTS=(
    -i "$CLIENT_KEY"
    -o "ProxyCommand=$RSP_SSH_BIN $RSP_SSH_CONF"
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o BatchMode=yes
    -o ConnectTimeout=30
    -o LogLevel=ERROR
)

T2_OK=true
if scp "${SCP_OPTS[@]}" "$LARGE_FILE_LOCAL" "$SSH_TARGET:$REMOTE_LARGE" 2>/dev/null; then
    # Retrieve back and verify
    if scp "${SCP_OPTS[@]}" "$SSH_TARGET:$REMOTE_LARGE" "$LARGE_FILE_RECV" 2>/dev/null; then
        RECV_SHA=$(sha256sum "$LARGE_FILE_RECV" | awk '{print $1}')
        if [[ "$LOCAL_SHA" == "$RECV_SHA" ]]; then
            pass "10 MB SCP round-trip checksum match"
        else
            fail "10 MB SCP round-trip checksum MISMATCH (sent=$LOCAL_SHA recv=$RECV_SHA)"
            T2_OK=false
        fi
    else
        fail "10 MB SCP: download failed"
        T2_OK=false
    fi
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "rm -f $REMOTE_LARGE" 2>/dev/null || true
else
    fail "10 MB SCP: upload failed"
    T2_OK=false
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 3: Rapid connect/disconnect (20 sequential)
# ─────────────────────────────────────────────────────────────────────────────
log "TEST 3: 20 rapid connect/disconnect sessions"
T3_FAILURES=0
for i in $(seq 1 20); do
    if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" 'true' 2>/dev/null; then
        T3_FAILURES=$((T3_FAILURES + 1))
        log "  rapid session $i failed"
    fi
done
if [[ $T3_FAILURES -eq 0 ]]; then
    pass "20 rapid connect/disconnect (all succeeded)"
else
    fail "20 rapid connect/disconnect ($T3_FAILURES/20 failed)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 4: Concurrent SCP uploads (5 × 2 MB)
# ─────────────────────────────────────────────────────────────────────────────
log "TEST 4: 5 concurrent SCP uploads (2 MB each)"
CONCURRENT_SCP=5
declare -a SCP_PIDS SCP_LOCAL SCP_SHAS
for i in $(seq 0 $((CONCURRENT_SCP - 1))); do
    SCP_LOCAL[$i]="$WORK/upload_$i.bin"
    dd if=/dev/urandom of="${SCP_LOCAL[$i]}" bs=1M count=2 2>/dev/null
    SCP_SHAS[$i]=$(sha256sum "${SCP_LOCAL[$i]}" | awk '{print $1}')
    scp "${SCP_OPTS[@]}" "${SCP_LOCAL[$i]}" "$SSH_TARGET:/tmp/rsp_stress_upload_${$}_$i.bin" >/dev/null 2>&1 &
    SCP_PIDS[$i]=$!
done

T4_FAILURES=0
for i in $(seq 0 $((CONCURRENT_SCP - 1))); do
    if ! wait "${SCP_PIDS[$i]}" 2>/dev/null; then
        T4_FAILURES=$((T4_FAILURES + 1))
        log "  SCP upload $i failed"
        continue
    fi
    # Verify integrity by downloading and checking
    recv="$WORK/upload_recv_$i.bin"
    if scp "${SCP_OPTS[@]}" "$SSH_TARGET:/tmp/rsp_stress_upload_${$}_$i.bin" "$recv" 2>/dev/null; then
        got_sha=$(sha256sum "$recv" | awk '{print $1}')
        if [[ "$got_sha" != "${SCP_SHAS[$i]}" ]]; then
            T4_FAILURES=$((T4_FAILURES + 1))
            log "  SCP upload $i: checksum mismatch"
        fi
    else
        T4_FAILURES=$((T4_FAILURES + 1))
        log "  SCP upload $i: verification download failed"
    fi
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "rm -f /tmp/rsp_stress_upload_${$}_$i.bin" 2>/dev/null || true
done
if [[ $T4_FAILURES -eq 0 ]]; then
    pass "5 concurrent SCP uploads (all correct)"
else
    fail "5 concurrent SCP uploads ($T4_FAILURES/$CONCURRENT_SCP failed)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 5: High-throughput pipe (50 MB through ssh cat)
# ─────────────────────────────────────────────────────────────────────────────
log "TEST 5: 50 MB pipe through ssh cat (throughput)"
PIPE_MB=50
PIPE_OUT="$WORK/pipe_recv.bin"
T5_START=$SECONDS
if dd if=/dev/zero bs=1M count=$PIPE_MB 2>/dev/null \
    | ssh "${SSH_OPTS[@]}" "$SSH_TARGET" 'cat' \
    >"$PIPE_OUT" 2>/dev/null; then
    T5_ELAPSED=$((SECONDS - T5_START))
    PIPE_RECV_SIZE=$(stat -c%s "$PIPE_OUT" 2>/dev/null || wc -c <"$PIPE_OUT")
    EXPECTED_SIZE=$((PIPE_MB * 1024 * 1024))
    if [[ "$PIPE_RECV_SIZE" -eq "$EXPECTED_SIZE" ]]; then
        if [[ $T5_ELAPSED -gt 0 ]]; then
            THROUGHPUT_MBS=$(( PIPE_MB / T5_ELAPSED ))
            pass "50 MB pipe (${T5_ELAPSED}s, ~${THROUGHPUT_MBS} MB/s)"
        else
            pass "50 MB pipe (<1s)"
        fi
    else
        fail "50 MB pipe: size mismatch (got ${PIPE_RECV_SIZE}, expected ${EXPECTED_SIZE})"
    fi
else
    fail "50 MB pipe: ssh cat command failed"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
TOTAL=$((PASS + FAIL))
echo ""
echo "========================================"
echo "openssh_stress_integration results"
echo "  PASS: $PASS / $TOTAL"
echo "  FAIL: $FAIL / $TOTAL"
echo "  elapsed: $((SECONDS - TEST_START_S))s"
echo "========================================"

if [[ $FAIL -eq 0 ]]; then
    echo "openssh_stress_integration passed"
    exit 0
else
    echo "openssh_stress_integration FAILED"
    exit 1
fi
