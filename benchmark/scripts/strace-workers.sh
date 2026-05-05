#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/../bin" && pwd)"
pkill -f "tcp-xylem" 2>/dev/null || true
sleep 1

echo "=== 4 workers: strace epoll_wait per thread (5s) ==="
strace -c -f -e trace=epoll_wait,epoll_ctl "$BIN/tcp-xylem-echo-mt" 9000 4 > /dev/null 2>/tmp/strace-4w.txt &
PID=$!
sleep 2
"$BIN/tcp-bench" throughput -n 1000 -d 5 -p 9000 2>/dev/null | grep throughput || true
sleep 1
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
echo "---"
tail -20 /tmp/strace-4w.txt

echo ""
echo "=== 1 worker: strace epoll_wait (5s) ==="
pkill -f "tcp-xylem" 2>/dev/null || true
sleep 1
strace -c -f -e trace=epoll_wait,epoll_ctl "$BIN/tcp-xylem-echo-mt" 9000 1 > /dev/null 2>/tmp/strace-1w.txt &
PID=$!
sleep 2
"$BIN/tcp-bench" throughput -n 1000 -d 5 -p 9000 2>/dev/null | grep throughput || true
sleep 1
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
echo "---"
tail -20 /tmp/strace-1w.txt
