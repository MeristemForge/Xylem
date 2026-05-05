#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/../bin" && pwd)"
PORT=9000
DUR=20
WORKERS=8

pkill -f "tcp-.*-echo" 2>/dev/null || true
sleep 1
ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true

SERVERS="xylem go rust"

echo "=== TCP Multi-Worker Benchmark: ${DUR}s, 64B payload ==="
echo "=== All servers: ${WORKERS} workers ==="
echo ""

for conns in 1000 10000; do
    label="${conns}"
    [ "$conns" -ge 1000 ] && label="$((conns/1000))k"
    echo "--- Throughput c${label} ---"
    for name in $SERVERS; do
        bin="$BIN/tcp-${name}-echo-mt"
        if [ ! -f "$bin" ]; then printf "%-8s SKIP\n" "$name"; continue; fi
        "$bin" $PORT $WORKERS >/dev/null 2>&1 &
        pid=$!
        sleep 3
        result=$("$BIN/tcp-bench" throughput -n $conns -d $DUR -p $PORT 2>/dev/null || true)
        kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
        sleep 1
        if [ -n "$result" ]; then
            tp=$(echo "$result" | grep throughput_msg_per_sec | grep -o "[0-9]*")
            p50=$(echo "$result" | grep latency_p50_us | grep -o "[0-9]*")
            p99=$(echo "$result" | grep latency_p99_us | grep -o "[0-9]*")
            max=$(echo "$result" | grep latency_max_us | grep -o "[0-9]*")
            printf "%-8s %8s msg/s  p50=%6s us  p99=%6s us  max=%6s us\n" "$name" "$tp" "$p50" "$p99" "$max"
        else
            printf "%-8s FAILED\n" "$name"
        fi
    done
    echo ""
done

echo "--- Connection Rate (concurrency=512) ---"
for name in $SERVERS; do
    bin="$BIN/tcp-${name}-echo-mt"
    if [ ! -f "$bin" ]; then printf "%-8s SKIP\n" "$name"; continue; fi
    "$bin" $PORT $WORKERS >/dev/null 2>&1 &
    pid=$!
    sleep 3
    result=$("$BIN/tcp-bench" connrate -c 512 -d $DUR -p $PORT 2>/dev/null || true)
    kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
    sleep 1
    if [ -n "$result" ]; then
        cps=$(echo "$result" | grep connects_per_sec | grep -o "[0-9]*")
        ok=$(echo "$result" | grep total_connects | grep -o "[0-9]*")
        fail=$(echo "$result" | grep failed_connects | grep -o "[0-9]*")
        printf "%-8s %8s conn/s  (ok=%s fail=%s)\n" "$name" "$cps" "$ok" "$fail"
    else
        printf "%-8s FAILED\n" "$name"
    fi
done
echo ""
echo "=== Done ==="
