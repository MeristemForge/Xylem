#!/usr/bin/env bash
# Run a given server and measure how much CPU the client itself
# uses. If the client is CPU-bound at 100%, scheduler changes on
# the server won't move throughput.
set -uo pipefail

BIN="$(cd "$(dirname "$0")"/.. && pwd)/bin"
PORT=9700
DUR="${DUR:-6}"
CONNS="${CONNS:-1000}"
SIZE="${SIZE:-64}"
WORKERS="${WORKERS:-$(nproc)}"
LABEL="${1:-xylem}"

case "$LABEL" in
    xylem) srv="$BIN/tcp-xylem-echo-mt" ;;
    rust)  srv="$BIN/tcp-rust-echo-mt" ;;
    go)    srv="$BIN/tcp-go-echo-mt" ;;
    *) echo "usage: $0 [xylem|rust|go]" >&2 ; exit 1 ;;
esac

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true
pkill -9 -f 'tcp-.*-echo' 2>/dev/null || true
sleep 1

"$srv" "$PORT" "$WORKERS" >/dev/null 2>&1 &
SPID=$!
sleep 2

/usr/bin/time -v "$BIN/tcp-bench" throughput -n "$CONNS" -d "$DUR" -s "$SIZE" -p "$PORT" 2>/tmp/client-time.out >/tmp/client-out.json

kill -TERM "$SPID" 2>/dev/null || true

echo "=== $LABEL conns=$CONNS size=$SIZE dur=${DUR}s ==="
grep -E 'throughput|p50|p99' /tmp/client-out.json || true
echo "--- client CPU ---"
grep -E 'User time|System time|Percent of CPU|Elapsed' /tmp/client-time.out || true
