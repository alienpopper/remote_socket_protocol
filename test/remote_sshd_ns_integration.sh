#!/usr/bin/env bash
# remote_sshd_ns_integration.sh — SSH over RSP with name-service discovery
#
# Architecture:
#   - RM + ES + NS run on THIS macOS box (172.16.206.1)
#   - rsp_sshd runs on the REMOTE Linux box (172.16.206.185)
#     It connects back to the macOS RM and registers itself as "linux-sshd"
#     with the name service.
#   - rsp_ssh runs on THIS macOS box as an OpenSSH ProxyCommand.
#     Instead of a hardcoded node ID, it looks up "linux-sshd" in the NS.
#
# Test cases:
#   1. Baseline SSH connection via name lookup (echo rsp-ns-ok)
#   2. 10 concurrent SSH sessions via name lookup
#   3. Large file SCP round-trip (5 MB) with checksum verification
#   4. 20 rapid connect/disconnect sessions
#
# Usage:
#   bash test/remote_sshd_ns_integration.sh [BIN_DIR] [LINUX_SSHD_BIN]
#
# BIN_DIR          defaults to ./build/bin (relative to repo root)
# LINUX_SSHD_BIN   defaults to ~/rsp_build/build/bin/rsp_sshd (on the Linux box)

set -euo pipefail

# ── paths ────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="${1:-$REPO_ROOT/build/bin}"
LINUX_SSHD_BIN="${2:-~/rsp_build/build/bin/rsp_sshd}"

LINUX_HOST="172.16.206.185"
MACOS_ADDR="172.16.206.1"
LINUX_USER="anonpoet"
SSH_TARGET_USER="anonpoet"
SSH_HOSTNAME="rsp-host"   # meaningless hostname; ProxyCommand handles routing
SSHD_SERVICE_NAME="linux-sshd"

RM_BIN="$BIN_DIR/resource_manager"
ES_BIN="$BIN_DIR/endorsement_service"
NS_BIN="$BIN_DIR/rsp_name_service"
RSP_SSH_BIN="$BIN_DIR/rsp_ssh"

for bin in "$RM_BIN" "$ES_BIN" "$NS_BIN" "$RSP_SSH_BIN"; do
    [[ -x "$bin" ]] || { echo "FAIL: not executable: $bin"; exit 1; }
done

# Verify we can reach the Linux box
ssh -o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=no \
    "$LINUX_USER@$LINUX_HOST" true 2>/dev/null \
    || { echo "FAIL: cannot SSH to $LINUX_HOST"; exit 1; }

# ── workspace ────────────────────────────────────────────────────────────────
WORK="$(mktemp -d "$REPO_ROOT/rsp_remote_ns_test.XXXXXX")"
RM_LOG="$WORK/rm.log"
ES_LOG="$WORK/es.log"
NS_LOG="$WORK/ns.log"
CLIENT_KEY="$WORK/id_test"
RSP_SSH_CONF="$WORK/rsp_ssh.conf.json"
LARGE_FILE_LOCAL="$WORK/large_local.bin"
LARGE_FILE_RECV="$WORK/large_recv.bin"

LINUX_WORK=""
RM_PID="" ES_PID="" NS_PID=""
PASS=0 FAIL=0
TEST_START_S=$SECONDS

# ── helpers ──────────────────────────────────────────────────────────────────
log()  { echo "[remote-sshd-ns][+$((SECONDS - TEST_START_S))s] $*"; }
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

wait_for_remote_log_line() {
    local pattern="$1" timeout_s="${2:-40}"
    local deadline=$((SECONDS + timeout_s))
    while [[ $SECONDS -lt $deadline ]]; do
        # Use printf %q to safely quote the pattern for the remote shell
        local qpat
        qpat=$(printf '%q' "$pattern")
        if ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
               "$LINUX_USER@$LINUX_HOST" \
               "grep -qF $qpat $LINUX_WORK/rsp_sshd.log 2>/dev/null"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

cleanup() {
    log "cleanup"
    if [[ -n "$LINUX_WORK" ]]; then
        ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
            "$LINUX_USER@$LINUX_HOST" \
            "PID=\$(cat $LINUX_WORK/rsp_sshd.pid 2>/dev/null); \
             [ -n \"\$PID\" ] && kill \"\$PID\" 2>/dev/null || true; \
             rm -rf $LINUX_WORK" 2>/dev/null || true
    fi
    [[ -n "$NS_PID" ]] && kill "$NS_PID" 2>/dev/null || true
    [[ -n "$ES_PID" ]] && kill "$ES_PID" 2>/dev/null || true
    [[ -n "$RM_PID" ]] && kill "$RM_PID" 2>/dev/null || true
    wait "$NS_PID" "$ES_PID" "$RM_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# ── pick a free port ─────────────────────────────────────────────────────────
free_port() {
    python3 -c "import socket; s=socket.socket(); s.bind(('',0)); p=s.getsockname()[1]; s.close(); print(p)"
}

# ── start RM ─────────────────────────────────────────────────────────────────
RM_PORT=$(free_port)
RM_LISTEN="0.0.0.0:$RM_PORT"
RM_ADVERTISE="$MACOS_ADDR:$RM_PORT"
RM_CONF="$WORK/rm.conf.json"
cat >"$RM_CONF" <<EOF
{"transports": ["tcp:$RM_LISTEN"]}
EOF
log "Starting RM on $RM_LISTEN (advertised as $RM_ADVERTISE)"
"$RM_BIN" --config "$RM_CONF" >"$RM_LOG" 2>&1 &
RM_PID=$!
wait_for_file_line "$RM_LOG" "listening on" 10 \
    || { echo "FAIL: RM did not start"; cat "$RM_LOG"; exit 1; }
log "RM up (pid=$RM_PID)"

# ── start ES ─────────────────────────────────────────────────────────────────
log "Starting ES"
"$ES_BIN" "tcp:127.0.0.1:$RM_PORT" >"$ES_LOG" 2>&1 &
ES_PID=$!
wait_for_file_line "$ES_LOG" "node ID:" 15 \
    || { echo "FAIL: ES did not emit node ID"; cat "$ES_LOG"; exit 1; }
ES_NODE_ID=$(grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' "$ES_LOG" | head -1)
[[ -n "$ES_NODE_ID" ]] || { echo "FAIL: could not parse ES node ID"; exit 1; }
log "ES up (pid=$ES_PID, node=$ES_NODE_ID)"

# ── start NS ─────────────────────────────────────────────────────────────────
NS_CONF="$WORK/ns.conf.json"
cat >"$NS_CONF" <<EOF
{"rm_servers": ["127.0.0.1:$RM_PORT"]}
EOF
log "Starting NS"
"$NS_BIN" --config "$NS_CONF" >"$NS_LOG" 2>&1 &
NS_PID=$!
wait_for_file_line "$NS_LOG" "connected to" 15 \
    || { echo "FAIL: NS did not connect"; cat "$NS_LOG"; exit 1; }
log "NS up (pid=$NS_PID)"

# ── generate client SSH key on this macOS box ─────────────────────────────────
ssh-keygen -t ed25519 -f "$CLIENT_KEY" -N "" -q
chmod 600 "$CLIENT_KEY"

# ── set up temp dir and keys on the Linux box ─────────────────────────────────
log "Creating workspace on $LINUX_HOST"
LINUX_WORK=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
    "$LINUX_USER@$LINUX_HOST" "mktemp -d /tmp/rsp_sshd_ns_test.XXXXXX")
log "Linux workspace: $LINUX_WORK"

ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$LINUX_USER@$LINUX_HOST" \
    "ssh-keygen -t ed25519 -f $LINUX_WORK/ssh_host_ed25519_key -N '' -q && \
     ssh-keygen -t rsa -b 2048 -f $LINUX_WORK/ssh_host_rsa_key -N '' -q"

scp -o BatchMode=yes -o StrictHostKeyChecking=no \
    "${CLIENT_KEY}.pub" "$LINUX_USER@$LINUX_HOST:$LINUX_WORK/authorized_keys"

SFTP_SERVER=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
    "$LINUX_USER@$LINUX_HOST" \
    'for p in /usr/lib/openssh/sftp-server /usr/libexec/sftp-server /usr/lib/ssh/sftp-server; do
         [ -x "$p" ] && echo "$p" && break
     done')
[[ -n "$SFTP_SERVER" ]] || { echo "FAIL: sftp-server not found on $LINUX_HOST"; exit 1; }
log "sftp-server on Linux: $SFTP_SERVER"

ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$LINUX_USER@$LINUX_HOST" \
    "cat > $LINUX_WORK/sshd_config" <<EOF
HostKey $LINUX_WORK/ssh_host_ed25519_key
HostKey $LINUX_WORK/ssh_host_rsa_key
AuthorizedKeysFile $LINUX_WORK/authorized_keys
StrictModes no
PubkeyAuthentication yes
PasswordAuthentication no
ChallengeResponseAuthentication no
UsePAM no
PrintMotd no
Subsystem sftp $SFTP_SERVER
EOF

# rsp_sshd config: includes ns_hostname so it registers with NS on startup
ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$LINUX_USER@$LINUX_HOST" \
    "cat > $LINUX_WORK/rsp_sshd.conf.json" <<EOF
{
  "rsp_transport": "tcp:$RM_ADVERTISE",
  "sshd_path": "/usr/sbin/sshd",
  "sshd_config": "$LINUX_WORK/sshd_config",
  "sshd_debug": false,
  "ns_hostname": "$SSHD_SERVICE_NAME"
}
EOF

# ── start rsp_sshd on the Linux box ──────────────────────────────────────────
log "Starting rsp_sshd on $LINUX_HOST (will register as '$SSHD_SERVICE_NAME')"
ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$LINUX_USER@$LINUX_HOST" \
    "nohup $LINUX_SSHD_BIN $LINUX_WORK/rsp_sshd.conf.json \
         > $LINUX_WORK/rsp_sshd.log 2>&1 & echo \$! > $LINUX_WORK/rsp_sshd.pid"

# Wait for rsp_sshd to register its hostname with NS
wait_for_remote_log_line "Registered hostname '$SSHD_SERVICE_NAME'" 30 \
    || {
        echo "FAIL: rsp_sshd on $LINUX_HOST did not register hostname with NS"
        echo "--- remote rsp_sshd log ---"
        ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
            "$LINUX_USER@$LINUX_HOST" "cat $LINUX_WORK/rsp_sshd.log" 2>/dev/null || true
        exit 1
    }
log "rsp_sshd registered '$SSHD_SERVICE_NAME' with NS"

# ── rsp_ssh client config: use service name, no hardcoded node ID ─────────────
cat >"$RSP_SSH_CONF" <<EOF
{
  "rsp_transport": "tcp:127.0.0.1:$RM_PORT",
  "resource_service_name": "$SSHD_SERVICE_NAME",
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
    -o ConnectTimeout=20
    -o LogLevel=ERROR
)
SCP_OPTS=(
    -i "$CLIENT_KEY"
    -o "ProxyCommand=$RSP_SSH_BIN $RSP_SSH_CONF"
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o BatchMode=yes
    -o ConnectTimeout=30
    -o LogLevel=ERROR
)
SSH_TARGET="$SSH_TARGET_USER@$SSH_HOSTNAME"

# ── verify baseline ───────────────────────────────────────────────────────────
log "Baseline connection check (name-based lookup)"
if output=$(ssh "${SSH_OPTS[@]}" "$SSH_TARGET" 'echo rsp-ns-ok' 2>/dev/null) \
        && [[ "$output" == "rsp-ns-ok" ]]; then
    pass "baseline SSH via name lookup"
else
    echo "FAIL: baseline SSH failed — output: '$output'"
    echo "--- remote rsp_sshd log ---"
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
        "$LINUX_USER@$LINUX_HOST" "cat $LINUX_WORK/rsp_sshd.log" 2>/dev/null || true
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 1: Concurrent SSH sessions (10 parallel) via name lookup
# ─────────────────────────────────────────────────────────────────────────────
log "TEST 1: 10 concurrent SSH sessions via name lookup"
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
    pass "10 concurrent sessions via name lookup (all correct output)"
else
    fail "10 concurrent sessions via name lookup ($T1_FAILURES/$CONCURRENT failed)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 2: Large file SCP round-trip (5 MB)
# ─────────────────────────────────────────────────────────────────────────────
log "TEST 2: Large file SCP round-trip (5 MB)"
dd if=/dev/urandom of="$LARGE_FILE_LOCAL" bs=1M count=5 2>/dev/null
LOCAL_SHA=$(shasum -a 256 "$LARGE_FILE_LOCAL" | awk '{print $1}')
REMOTE_LARGE="/tmp/rsp_remote_ns_large_$$.bin"

if scp "${SCP_OPTS[@]}" "$LARGE_FILE_LOCAL" "$SSH_TARGET:$REMOTE_LARGE" 2>/dev/null; then
    if scp "${SCP_OPTS[@]}" "$SSH_TARGET:$REMOTE_LARGE" "$LARGE_FILE_RECV" 2>/dev/null; then
        RECV_SHA=$(shasum -a 256 "$LARGE_FILE_RECV" | awk '{print $1}')
        if [[ "$LOCAL_SHA" == "$RECV_SHA" ]]; then
            pass "5 MB SCP round-trip checksum match"
        else
            fail "5 MB SCP round-trip checksum MISMATCH"
        fi
    else
        fail "5 MB SCP: download failed"
    fi
    ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "rm -f $REMOTE_LARGE" 2>/dev/null || true
else
    fail "5 MB SCP: upload failed"
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 3: Rapid connect/disconnect (20 sequential)
# ─────────────────────────────────────────────────────────────────────────────
log "TEST 3: 20 rapid connect/disconnect sessions via name lookup"
T3_FAILURES=0
for i in $(seq 1 20); do
    if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" 'true' 2>/dev/null; then
        T3_FAILURES=$((T3_FAILURES + 1))
        log "  rapid session $i failed"
    fi
done
if [[ $T3_FAILURES -eq 0 ]]; then
    pass "20 rapid connect/disconnect via name lookup (all succeeded)"
else
    fail "20 rapid connect/disconnect via name lookup ($T3_FAILURES/20 failed)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
TOTAL=$((PASS + FAIL))
echo ""
echo "========================================"
echo "remote_sshd_ns_integration results"
echo "  PASS: $PASS / $TOTAL"
echo "  FAIL: $FAIL / $TOTAL"
echo "  elapsed: $((SECONDS - TEST_START_S))s"
echo "========================================"

if [[ $FAIL -eq 0 ]]; then
    echo "remote_sshd_ns_integration passed"
    exit 0
else
    echo "remote_sshd_ns_integration FAILED"
    exit 1
fi
