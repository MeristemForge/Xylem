#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$BENCH_DIR/bin"
PORT=9000
DURATION=10
CONNS=1000

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true

SERVERS="xylem libuv libevent libhv boost go rust"

echo ""
echo "=== TCP Echo Throughput: ${CONNS} connections, ${DURATION}s, 64B payload ==="
echo ""

for name in $SERVERS; do
    bin="$BIN/tcp-${name}-echo"
    if [ ! -f "$bin" ]; then
        printf "%-12s SKIP (not found)\n" "$name"
        continue
    fi

    "$bin" $PORT >/dev/null 2>&1 &
    pid=$!
    sleep 2

    result=$("$BIN/tcp-bench" throughput -n $CONNS -d $DURATION -p $PORT 2>/dev/null || true)

    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true

    if [ -n "$result" ]; then
        tp=$(echo "$result" | grep throughput_msg_per_sec | grep -o "[0-9]*")
        p50=$(echo "$result" | grep latency_p50_us | grep -o "[0-9]*")
        p99=$(echo "$result" | grep latency_p99_us | grep -o "[0-9]*")
        printf "%-12s %8s msg/s  p50=%5s us  p99=%5s us\n" "$name" "$tp" "$p50" "$p99"
    else
        printf "%-12s FAILED (no output)\n" "$name"
    fi

    sleep 1
done

echo ""
echo "=== TCP Connection Rate: concurrency=512, ${DURATION}s ==="
echo ""

for name in $SERVERS; do
    bin="$BIN/tcp-${name}-echo"
    if [ ! -f "$bin" ]; then
        printf "%-12s SKIP (not found)\n" "$name"
        continue
    fi

    "$bin" $PORT >/dev/null 2>&1 &
    pid=$!
    sleep 2

    result=$("$BIN/tcp-bench" connrate -c 512 -d $DURATION -p $PORT 2>/dev/null || true)

    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true

    if [ -n "$result" ]; then
        cps=$(echo "$result" | grep connects_per_sec | grep -o "[0-9]*")
        printf "%-12s %8s conn/s\n" "$name" "$cps"
    else
        printf "%-12s FAILED (no output)\n" "$name"
    fi

    sleep 1
done

echo ""
echo "=== Done ==="
