#!/bin/sh
# FluxPort integration tests.  Runs from the build directory and expects
# FLUXPORT_SERVER, FLUXPORT_CLIENT and FLUXPORT_TESTTOOL to point at built
# executables (make test supplies them).
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
pick_exe() {
    if [ -x "$1" ]; then
        echo "$1"
    elif [ -x "$1.exe" ]; then
        echo "$1.exe"
    else
        echo "$1"
    fi
}
SERVER=${FLUXPORT_SERVER:-$(pick_exe "$ROOT/fluxport-server")}
CLIENT=${FLUXPORT_CLIENT:-$(pick_exe "$ROOT/fluxport-client")}
TOOL=${FLUXPORT_TESTTOOL:-$(pick_exe "$ROOT/fp_testtool")}

WORK=${FLUXPORT_TEST_WORK:-"$ROOT/tests/.tmp.$$"}
rm -rf "$WORK"
mkdir -p "$WORK"
PIDS=""

track() {
    PIDS="$PIDS $1"
}

stop_pid() {
    if [ -n "$1" ]; then
        kill "$1" 2>/dev/null || true
        wait "$1" 2>/dev/null || true
    fi
}

cleanup() {
    for p in $PIDS; do
        kill "$p" 2>/dev/null || true
    done
    for p in $PIDS; do
        wait "$p" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    echo "PASS: $*"
}

wait_log() {
    file=$1
    pattern=$2
    n=0
    while [ "$n" -lt 60 ]; do
        if [ -f "$file" ] && grep -q "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        n=$((n + 1))
    done
    return 1
}

run_expect() {
    expected=$1
    shift
    set +e
    "$@"
    code=$?
    set -e
    if [ "$code" -ne "$expected" ]; then
        fail "expected exit $expected, got $code: $*"
    fi
}

write_client() {
    file=$1
    port=$2
    token=$3
    keepalive=$4
    tunnel_line=$5
    {
        echo "server 127.0.0.1:$port"
        echo "token $token"
        echo "name test-client"
        echo "keepalive $keepalive"
        echo "connect_timeout 3"
        echo "log_level debug"
        if [ -n "$tunnel_line" ]; then
            echo "$tunnel_line"
        fi
    } >"$file"
}

probe_expect() {
    port=$1
    message=$2
    expected=$3
    set +e
    out=$("$TOOL" probe 127.0.0.1 "$port" "$message" 3000 2>"$WORK/probe.err")
    code=$?
    set -e
    if [ "$code" -ne 0 ]; then
        fail "probe $port '$message' failed with $code: $(cat "$WORK/probe.err")"
    fi
    if [ "$out" != "$expected" ]; then
        fail "probe $port '$message': expected '$expected', got '$out'"
    fi
}

probe_fails() {
    port=$1
    set +e
    "$TOOL" probe 127.0.0.1 "$port" "must-not-arrive" 700 \
        >"$WORK/probe-fail.out" 2>/dev/null
    code=$?
    set -e
    if [ "$code" -eq 0 ]; then
        fail "unexpected UDP response on closed port $port"
    fi
}

fake_test() {
    port=$1
    mode=$2
    expected=$3
    keepalive=$4
    tunnel_line=$5
    cfg="$WORK/fake-$mode.conf"
    write_client "$cfg" "$port" "x" "$keepalive" "$tunnel_line"
    "$TOOL" fake-server "$port" "$mode" \
        >"$WORK/fake-$mode.log" 2>&1 &
    fpid=$!
    track "$fpid"
    if ! wait_log "$WORK/fake-$mode.log" "READY"; then
        fail "fake server $mode did not start: $(cat "$WORK/fake-$mode.log")"
    fi
    run_expect "$expected" "$CLIENT" -c "$cfg" run
    stop_pid "$fpid"
}

# ---------------------------------------------------------------------------
# 1. Persistent tunnel table commands.
# ---------------------------------------------------------------------------
CFG="$WORK/tunnels.conf"
cat >"$CFG" <<EOF
# keep this comment
server 127.0.0.1:1
EOF

"$CLIENT" -c "$CFG" add -L 127.0.0.1:39040 127.0.0.1:39041 >/dev/null
"$CLIENT" -c "$CFG" add -R 127.0.0.1:39042 127.0.0.1:39043 >/dev/null
if ! grep -q "keep this comment" "$CFG"; then
    fail "add did not preserve comments"
fi
LIST=$("$CLIENT" -c "$CFG" list)
case "$LIST" in
    *"1. L 127.0.0.1:39040 127.0.0.1:39041"*"2. R 127.0.0.1:39042 127.0.0.1:39043"*)
        ;;
    *)
        fail "unexpected list output: $LIST"
        ;;
esac
if "$CLIENT" -c "$CFG" add -L 127.0.0.1:39040 127.0.0.1:39041 >/dev/null 2>&1; then
    fail "duplicate tunnel add was accepted"
fi
"$CLIENT" -c "$CFG" remove 127.0.0.1:39042 >/dev/null
LIST=$("$CLIENT" -c "$CFG" list)
case "$LIST" in
    *"R 127.0.0.1:39042"*)
        fail "prefix remove left the R tunnel: $LIST"
        ;;
esac
"$CLIENT" -c "$CFG" remove 1 >/dev/null
LIST=$("$CLIENT" -c "$CFG" list)
case "$LIST" in
    *"no tunnels configured"*) ;;
    *) fail "index remove left a tunnel: $LIST" ;;
esac
pass "config add/list/remove"

# ---------------------------------------------------------------------------
# 2. CLI and authentication failures.
# ---------------------------------------------------------------------------
if "$CLIENT" --definitely-not-an-option >/dev/null 2>&1; then
    fail "unknown option was accepted"
fi
set +e
"$CLIENT" run >/dev/null 2>&1
code=$?
set -e
if [ "$code" -ne 1 ]; then
    fail "client without server should exit 1, got $code"
fi

AUTH_PORT=39010
cat >"$WORK/auth-server.conf" <<EOF
listen 127.0.0.1:$AUTH_PORT
token secret
allow_ports all
keepalive 15
udp_idle 2
log_level info
EOF
"$SERVER" -c "$WORK/auth-server.conf" >"$WORK/auth-server.log" 2>&1 &
AUTH_PID=$!
track "$AUTH_PID"
if ! wait_log "$WORK/auth-server.log" "listening"; then
    fail "auth server did not start"
fi
write_client "$WORK/auth-client.conf" "$AUTH_PORT" "wrong" 15 ""
run_expect 2 "$CLIENT" -c "$WORK/auth-client.conf" run
stop_pid "$AUTH_PID"
pass "bad token exits 2"

# ---------------------------------------------------------------------------
# 3. Fake server protocol failures and heartbeat.
# ---------------------------------------------------------------------------
BAD_PORT=39011
fake_test "$BAD_PORT" bad-token 2 15 ""
fake_test 39012 bad-version 2 15 ""
fake_test 39013 bad-len 2 15 ""
fake_test 39014 half 2 15 ""
fake_test 39015 goaway 0 15 ""
fake_test 39016 silent 2 1 ""
fake_test 39017 partial-goaway 3 15 "R 127.0.0.1:39018 127.0.0.1:39019"
pass "client handshake, malformed frame and heartbeat handling"

# ---------------------------------------------------------------------------
# 4. Real L and R forwarding, concurrency, idle expiry, malformed clients.
# ---------------------------------------------------------------------------
SRV_PORT=39020
L_PORT=39021
L_TARGET=39022
R_PORT=39023
R_TARGET=39024

"$TOOL" echo 127.0.0.1 "$L_TARGET" "echo:" >"$WORK/echo-l.log" 2>&1 &
ECHO_L_PID=$!
track "$ECHO_L_PID"
"$TOOL" echo 127.0.0.1 "$R_TARGET" "echo:" >"$WORK/echo-r.log" 2>&1 &
ECHO_R_PID=$!
track "$ECHO_R_PID"
if ! wait_log "$WORK/echo-l.log" "READY" ||
   ! wait_log "$WORK/echo-r.log" "READY"; then
    fail "echo targets did not start"
fi

cat >"$WORK/server.conf" <<EOF
listen 127.0.0.1:$SRV_PORT
token secret
allow_ports $R_PORT
keepalive 15
udp_idle 2
log_level debug
EOF

cat >"$WORK/client.conf" <<EOF
server 127.0.0.1:$SRV_PORT
token secret
name e2e-client
keepalive 15
udp_timeout 2
log_level debug
L 127.0.0.1:$L_PORT 127.0.0.1:$L_TARGET
R 127.0.0.1:$R_PORT 127.0.0.1:$R_TARGET
EOF

"$SERVER" -c "$WORK/server.conf" >"$WORK/server.log" 2>&1 &
SERVER_PID=$!
track "$SERVER_PID"
if ! wait_log "$WORK/server.log" "listening"; then
    fail "server did not start"
fi
"$CLIENT" -c "$WORK/client.conf" run >"$WORK/client.log" 2>&1 &
CLIENT_PID=$!
track "$CLIENT_PID"
if ! wait_log "$WORK/client.log" "tunnel setup complete"; then
    fail "client did not finish tunnel setup: $(cat "$WORK/client.log")"
fi

probe_expect "$L_PORT" "hello-l" "echo:hello-l"
probe_expect "$R_PORT" "hello-r" "echo:hello-r"

i=1
CPIDS=""
while [ "$i" -le 4 ]; do
    ("$TOOL" probe 127.0.0.1 "$L_PORT" "concurrent-$i" 3000 \
        >"$WORK/concurrent-$i.out" 2>&1
     echo $? >"$WORK/concurrent-$i.rc") &
    CPIDS="$CPIDS $!"
    i=$((i + 1))
done
for p in $CPIDS; do
    wait "$p" || true
done
i=1
while [ "$i" -le 4 ]; do
    if [ "$(cat "$WORK/concurrent-$i.rc")" -ne 0 ]; then
        fail "concurrent probe $i failed"
    fi
    if [ "$(cat "$WORK/concurrent-$i.out")" != "echo:concurrent-$i" ]; then
        fail "concurrent probe $i wrong reply: $(cat "$WORK/concurrent-$i.out")"
    fi
    i=$((i + 1))
done
pass "L/R forwarding and concurrent sessions"

sleep 3.2
probe_expect "$L_PORT" "after-idle-l" "echo:after-idle-l"
probe_expect "$R_PORT" "after-idle-r" "echo:after-idle-r"
pass "idle session expiry and re-create"

cat >"$WORK/multi-client.conf" <<EOF
server 127.0.0.1:$SRV_PORT
token secret
name second-client
keepalive 15
udp_idle 2
log_level debug
L 127.0.0.1:39025 127.0.0.1:$L_TARGET
EOF
"$CLIENT" -c "$WORK/multi-client.conf" run >"$WORK/multi-client.log" 2>&1 &
MULTI_PID=$!
track "$MULTI_PID"
if ! wait_log "$WORK/multi-client.log" "tunnel setup complete"; then
    fail "second concurrent client did not start"
fi
probe_expect 39025 "multi-client" "echo:multi-client"
probe_expect "$L_PORT" "first-client-still-works" "echo:first-client-still-works"
probe_expect "$R_PORT" "first-r-still-works" "echo:first-r-still-works"
stop_pid "$MULTI_PID"
probe_expect "$L_PORT" "first-after-second" "echo:first-after-second"
pass "multiple clients with independent tunnels"

"$TOOL" malicious 127.0.0.1 "$SRV_PORT" huge >/dev/null 2>&1
"$TOOL" malicious 127.0.0.1 "$SRV_PORT" half >/dev/null 2>&1
sleep 0.3
probe_expect "$L_PORT" "after-malformed" "echo:after-malformed"
probe_expect "$R_PORT" "after-malformed-r" "echo:after-malformed-r"
pass "server survives malformed clients"

stop_pid "$CLIENT_PID"
sleep 1.0
probe_fails "$L_PORT"
probe_fails "$R_PORT"

"$CLIENT" -c "$WORK/client.conf" run >"$WORK/client2.log" 2>&1 &
CLIENT2_PID=$!
track "$CLIENT2_PID"
if ! wait_log "$WORK/client2.log" "tunnel setup complete"; then
    fail "second client did not start: $(cat "$WORK/client2.log")"
fi
probe_expect "$L_PORT" "restart-l" "echo:restart-l"
probe_expect "$R_PORT" "restart-r" "echo:restart-r"
stop_pid "$CLIENT2_PID"
stop_pid "$SERVER_PID"
stop_pid "$ECHO_L_PID"
stop_pid "$ECHO_R_PID"
pass "disconnect cleanup and listener port release"

# ---------------------------------------------------------------------------
# 5. Strict partial tunnel failure against a real server.
# ---------------------------------------------------------------------------
STRICT_PORT=39030
STRICT_ALLOWED=39031
STRICT_DENIED=39032
cat >"$WORK/strict-server.conf" <<EOF
listen 127.0.0.1:$STRICT_PORT
token secret
allow_ports $STRICT_ALLOWED
keepalive 15
udp_idle 2
log_level info
EOF
"$SERVER" -c "$WORK/strict-server.conf" >"$WORK/strict-server.log" 2>&1 &
STRICT_PID=$!
track "$STRICT_PID"
if ! wait_log "$WORK/strict-server.log" "listening"; then
    fail "strict server did not start"
fi
cat >"$WORK/strict-client.conf" <<EOF
server 127.0.0.1:$STRICT_PORT
token secret
strict on
connect_timeout 3
log_level warn
R 127.0.0.1:$STRICT_ALLOWED 127.0.0.1:39033
R 127.0.0.1:$STRICT_DENIED 127.0.0.1:39034
EOF
run_expect 3 "$CLIENT" -c "$WORK/strict-client.conf" run
stop_pid "$STRICT_PID"
pass "strict mode exits 3 on partial tunnel failure"

# ---------------------------------------------------------------------------
# 6. Server heartbeat against a silent authenticated client.
# ---------------------------------------------------------------------------
HB_PORT=39040
cat >"$WORK/hb-server.conf" <<EOF
listen 127.0.0.1:$HB_PORT
allow_ports all
keepalive 1
udp_idle 2
log_level info
EOF
"$SERVER" -c "$WORK/hb-server.conf" >"$WORK/hb-server.log" 2>&1 &
HB_PID=$!
track "$HB_PID"
if ! wait_log "$WORK/hb-server.log" "listening"; then
    fail "heartbeat server did not start"
fi
run_expect 0 "$TOOL" malicious 127.0.0.1 "$HB_PORT" silent-client
stop_pid "$HB_PID"
pass "server closes a silent authenticated client"

echo "All FluxPort tests passed."
