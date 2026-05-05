#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/../bin" && pwd)"
PORT=9200
DUR=10
CONNS=1000

extract() {
    local key="$1" json="$2"
    echo "$json" | sed -n "s/.*\"${key}\": *\([0-9]*\).*/\1/p"
}

echo "=== Windows TCP Multi-Worker Benchmark ==="
echo "=== ${CONNS} conns, ${DUR}s, 64B payload ==="
echo ""

for workers in 1 4 8; do
    echo "--- Workers: ${workers} ---"
    printf "%-8s %12s %10s %10s %10s\n" "Server" "msg/s" "p50(us)" "p99(us)" "max(us)"
    printf "%-8s %12s %10s %10s %10s\n" "------" "-----" "-------" "-------" "-------"

    for name in xylem go rust; do
        bin="$BIN/tcp-${name}-echo-mt.exe"
        if [ ! -f "$bin" ]; then printf "%-8s SKIP\n" "$name"; continue; fi

        "$bin" $PORT $workers >/dev/null 2>&1 &
        pid=$!
        sleep 3

        result=$("$BIN/tcp-bench-win.exe" throughput -n $CONNS -d $DUR -p $PORT 2>/dev/null || true)

        kill $pid 2>/dev/null || true
        wait $pid 2>/dev/null || true
        sleep 2

        if [ -n "$result" ]; then
            tp=$(extract "throughput_msg_per_sec" "$result")
            p50=$(extract "latency_p50_us" "$result")
            p99=$(extract "latency_p99_us" "$result")
            pmax=$(extract "latency_max_us" "$result")
            printf "%-8s %12s %10s %10s %10s\n" "$name" "$tp" "$p50" "$p99" "$pmax"
        else
            printf "%-8s FAILED\n" "$name"
        fi
    done
    echo ""
done

echo "=== Done ==="
