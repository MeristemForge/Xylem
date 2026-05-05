#!/usr/bin/env bash
set -euo pipefail
BIN="$(cd "$(dirname "$0")/../bin" && pwd)"
pkill -f "tcp-.*-echo" 2>/dev/null || true
sleep 1

echo "=== 4 workers, c1k, 5s — per-thread CPU ==="
"$BIN/tcp-xylem-echo-mt" 9000 4 >/dev/null 2>&1 &
PID=$!
sleep 2

# Start benchmark
"$BIN/tcp-bench" throughput -n 1000 -d 5 -p 9000 >/dev/null 2>/dev/null &
BENCH=$!

# Sample thread CPU at start
echo "Thread CPU (utime+stime) at start:"
for t in /proc/$PID/task/*/stat; do
    tid=$(basename $(dirname $t))
    utime=$(awk '{print $14}' $t)
    stime=$(awk '{print $15}' $t)
    echo "  tid=$tid utime=$utime stime=$stime"
done

sleep 5
wait $BENCH 2>/dev/null || true

# Sample thread CPU at end
echo ""
echo "Thread CPU (utime+stime) at end:"
for t in /proc/$PID/task/*/stat; do
    tid=$(basename $(dirname $t))
    utime=$(awk '{print $14}' $t)
    stime=$(awk '{print $15}' $t)
    echo "  tid=$tid utime=$utime stime=$stime"
done

kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
