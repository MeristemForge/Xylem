#!/usr/bin/env bash
# Rust MT echo syscall profile, matching strace-xylem-c10k-64k.sh.
set -uo pipefail

BIN_DIR="$(cd "$(dirname "$0")"/.. && pwd)/bin"
PORT=9601
DURATION="${DURATION:-6}"
WORKERS="${WORKERS:-$(nproc)}"

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true
pkill -9 -f "tcp-rust-echo" 2>/dev/null || true
sleep 1

rm -f /tmp/strace-rust-out.txt

setsid strace -f -c -o /tmp/strace-rust-out.txt \
    "$BIN_DIR/tcp-rust-echo-mt" "$PORT" "$WORKERS" >/dev/null 2>&1 &
STRACE_PID=$!

sleep 2
PID=$(pgrep -P "$STRACE_PID" -f "tcp-rust-echo-mt" | head -1 || true)
if [ -z "$PID" ]; then
    PID=$(pgrep -f "$BIN_DIR/tcp-rust-echo-mt" | head -1 || true)
fi
echo "=== rust pid=$PID (under strace pid=$STRACE_PID), duration=${DURATION}s ==="

"$BIN_DIR/tcp-bench" throughput -n 10000 -d "$DURATION" -s 65536 -p "$PORT" \
    >/tmp/rust-strace-bench.json 2>&1

if [ -n "$PID" ]; then
    kill -TERM "$PID" 2>/dev/null || true
    for _ in 1 2 3 4 5; do
        kill -0 "$PID" 2>/dev/null || break
        sleep 1
    done
    kill -KILL "$PID" 2>/dev/null || true
fi

for _ in 1 2 3 4 5; do
    kill -0 "$STRACE_PID" 2>/dev/null || break
    sleep 1
done

echo ""
echo "--- bench result ---"
grep -E 'throughput_msg|latency_p5|latency_p99' /tmp/rust-strace-bench.json || true

echo ""
echo "--- syscall totals (strace -c) ---"
if [ -s /tmp/strace-rust-out.txt ]; then
    cat /tmp/strace-rust-out.txt
else
    echo "(no strace output captured)"
fi
