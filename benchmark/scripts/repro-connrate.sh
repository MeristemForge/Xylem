#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/../bin" && pwd)"

pkill -f "tcp-.*-echo" 2>/dev/null || true
sleep 1

echo "=== 1 worker ==="
"$BIN/tcp-xylem-echo-mt" 9000 1 &>/dev/null &
PID=$!
sleep 2
"$BIN/tcp-bench" connrate -c 512 -d 5 -p 9000
kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
sleep 1

echo ""
echo "=== 4 workers ==="
"$BIN/tcp-xylem-echo-mt" 9000 4 &>/dev/null &
PID=$!
sleep 2
"$BIN/tcp-bench" connrate -c 512 -d 5 -p 9000
kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
sleep 1

echo ""
echo "=== 2 workers ==="
"$BIN/tcp-xylem-echo-mt" 9000 2 &>/dev/null &
PID=$!
sleep 2
"$BIN/tcp-bench" connrate -c 512 -d 5 -p 9000
kill $PID 2>/dev/null; wait $PID 2>/dev/null || true

echo ""
echo "=== Done ==="
