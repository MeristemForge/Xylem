#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/../bin" && pwd)"
PORT=9000
DUR=10

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true
pkill -f "tcp-xylem" 2>/dev/null || true
sleep 1

echo "=== Diagnosing multi-worker throughput issue ==="
echo "=== Comparing xylem vs go (both multi-threaded) ==="
echo ""

printf "%-8s %-8s %12s %12s %12s\n" "Server" "Workers" "Throughput" "P50(us)" "P99(us)"
printf "%-8s %-8s %12s %12s %12s\n" "------" "-------" "----------" "-------" "-------"

for conns in 1000; do
    for workers in 1 4; do
        # Xylem
        pkill -f "tcp-xylem" 2>/dev/null || true
        sleep 1
        "$BIN/tcp-xylem-echo-mt" $PORT $workers >/dev/null 2>&1 &
        pid=$!
        sleep 2
        result=$("$BIN/tcp-bench" throughput -n $conns -d $DUR -p $PORT 2>/dev/null || true)
        kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
        if [ -n "$result" ]; then
            tp=$(echo "$result" | grep -m1 throughput_msg_per_sec | grep -oP '"\K[0-9]+' || echo "$result" | grep throughput_msg_per_sec | grep -o "[0-9]*")
            p50=$(echo "$result" | grep -m1 latency_p50_us | grep -oP '"\K[0-9]+' || echo "$result" | grep latency_p50_us | grep -o "[0-9]*")
            p99=$(echo "$result" | grep -m1 latency_p99_us | grep -oP '"\K[0-9]+' || echo "$result" | grep latency_p99_us | grep -o "[0-9]*")
            printf "%-8s %-8s %12s %12s %12s\n" "xylem" "$workers" "$tp" "$p50" "$p99"
        else
            printf "%-8s %-8s %12s\n" "xylem" "$workers" "FAIL"
        fi

        # Go
        pkill -f "tcp-go" 2>/dev/null || true
        sleep 1
        if [ -f "$BIN/tcp-go-echo-mt" ]; then
            "$BIN/tcp-go-echo-mt" $PORT $workers >/dev/null 2>&1 &
            pid=$!
            sleep 2
            result=$("$BIN/tcp-bench" throughput -n $conns -d $DUR -p $PORT 2>/dev/null || true)
            kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
            if [ -n "$result" ]; then
                tp=$(echo "$result" | grep -m1 throughput_msg_per_sec | grep -oP '"\K[0-9]+' || echo "$result" | grep throughput_msg_per_sec | grep -o "[0-9]*")
                p50=$(echo "$result" | grep -m1 latency_p50_us | grep -oP '"\K[0-9]+' || echo "$result" | grep latency_p50_us | grep -o "[0-9]*")
                p99=$(echo "$result" | grep -m1 latency_p99_us | grep -oP '"\K[0-9]+' || echo "$result" | grep latency_p99_us | grep -o "[0-9]*")
                printf "%-8s %-8s %12s %12s %12s\n" "go" "$workers" "$tp" "$p50" "$p99"
            else
                printf "%-8s %-8s %12s\n" "go" "$workers" "FAIL"
            fi
        fi
    done
done

echo ""
echo "=== Checking: does xylem 4-worker actually use multiple threads? ==="
pkill -f "tcp-xylem" 2>/dev/null || true
sleep 1
"$BIN/tcp-xylem-echo-mt" $PORT 4 >/dev/null 2>&1 &
pid=$!
sleep 2

echo "Thread count: $(ls /proc/$pid/task 2>/dev/null | wc -l)"
echo "Running tcp-bench for 3s..."
"$BIN/tcp-bench" throughput -n 1000 -d 3 -p $PORT >/dev/null 2>/dev/null &
bench_pid=$!
sleep 1

echo ""
echo "Per-thread CPU usage (top snapshot):"
top -b -n 1 -H -p $pid 2>/dev/null | head -20 || ps -eLo pid,lwp,pcpu,comm -p $pid 2>/dev/null | head -10

wait $bench_pid 2>/dev/null || true
kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true

echo ""
echo "=== Done ==="
