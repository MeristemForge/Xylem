#!/usr/bin/env bash
# Generic strace -c collector across xylem / rust / go MT echo.
# Usage: strace-any.sh <label> [CONNS DUR SIZE]
set -uo pipefail

BIN="$(cd "$(dirname "$0")"/.. && pwd)/bin"
PORT=9800
LABEL="${1:-xylem}"
DUR="${DUR:-6}"
CONNS="${CONNS:-1000}"
SIZE="${SIZE:-64}"
WORKERS="${WORKERS:-$(nproc)}"

case "$LABEL" in
    xylem) srv="$BIN/tcp-xylem-echo-mt" ;;
    rust)  srv="$BIN/tcp-rust-echo-mt" ;;
    go)    srv="$BIN/tcp-go-echo-mt" ;;
    *) echo "usage: $0 [xylem|rust|go]" >&2 ; exit 1 ;;
esac

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true
pkill -9 -f 'tcp-.*-echo' 2>/dev/null || true
sleep 1

rm -f /tmp/strace-any.out

setsid strace -f -c -o /tmp/strace-any.out "$srv" "$PORT" "$WORKERS" >/dev/null 2>&1 &
STRACE_PID=$!

sleep 2
PID=$(pgrep -P "$STRACE_PID" -f "tcp-.*-echo-mt" | head -1 || true)
if [ -z "$PID" ]; then
    PID=$(pgrep -f "$srv" | head -1 || true)
fi

out="$("$BIN/tcp-bench" throughput -n "$CONNS" -d "$DUR" -s "$SIZE" -p "$PORT" 2>/dev/null)"

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

tp="$( echo "$out" | grep throughput_msg_per_sec | grep -oE '[0-9]+' | tail -1 )"
p50="$(echo "$out" | grep latency_p50_us         | grep -oE '[0-9]+' | tail -1 )"

echo "=== $LABEL conns=$CONNS size=${SIZE}B dur=${DUR}s ==="
printf 'throughput=%s msg/s  p50=%s us\n' "${tp:-?}" "${p50:-?}"
echo "--- syscall top 6 ---"
head -1 /tmp/strace-any.out
grep -E '^ *[0-9]+\.[0-9]+' /tmp/strace-any.out | head -6
