#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/../bin" && pwd)"
PORT=9000
DUR=10

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true
pkill -f "tcp-xylem" 2>/dev/null || true
sleep 1

echo "=== 10000 connections, ${DUR}s ==="
echo ""

for workers in 1 4 8; do
    pkill -f "tcp-xylem" 2>/dev/null || true
    sleep 1

    "$BIN/tcp-xylem-echo-mt" $PORT $workers >/dev/null 2>&1 &
    pid=$!
    sleep 2

    echo "--- $workers worker(s) ---"
    result=$("$BIN/tcp-bench" throughput -n 10000 -d $DUR -p $PORT 2>/dev/null || true)
    if [ -n "$result" ]; then
        echo "$result" | grep -E "throughput|latency_p50|latency_p99"
    else
        echo "FAILED - no output"
    fi

    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    echo ""
done

echo "=== Compare with Go (4 workers, 10k conns) ==="
pkill -f "tcp-go" 2>/dev/null || true
sleep 1
if [ -f "$BIN/tcp-go-echo-mt" ]; then
    "$BIN/tcp-go-echo-mt" $PORT 4 >/dev/null 2>&1 &
    pid=$!
    sleep 2
    result=$("$BIN/tcp-bench" throughput -n 10000 -d $DUR -p $PORT 2>/dev/null || true)
    if [ -n "$result" ]; then
        echo "$result" | grep -E "throughput|latency_p50|latency_p99"
    else
        echo "FAILED"
    fi
    kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
fi
