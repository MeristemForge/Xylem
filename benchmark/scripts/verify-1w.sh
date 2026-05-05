#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/../bin" && pwd)"
pkill -f "tcp-xylem" 2>/dev/null || true
sleep 1
ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true

"$BIN/tcp-xylem-echo-mt" 9000 1 >/dev/null 2>&1 &
pid=$!
sleep 2

echo "=== 1 worker, 3 runs ==="
for i in 1 2 3; do
    result=$("$BIN/tcp-bench" throughput -n 1000 -d 10 -p 9000 2>/dev/null || true)
    tp=$(echo "$result" | grep throughput_msg_per_sec | grep -o "[0-9]*")
    p50=$(echo "$result" | grep latency_p50_us | grep -o "[0-9]*")
    echo "  run$i: $tp msg/s  p50=$p50 us"
done

kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true
