#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/../out" || exit 1
pkill -f '/benchmark/out/.*-echo' 2>/dev/null
pkill -f 'tls-bench' 2>/dev/null
sleep 1
ulimit -n 200000 2>/dev/null || true

taskset -c 0 ./tls-xylem-echo 9556 >/dev/null 2>&1 &
P=$!
sleep 2
GOMAXPROCS=8 taskset -c 8-15 ./tls-bench throughput \
    -n 10000 -d 10 -s 65536 -p 9556 >/tmp/c.json 2>/dev/null
RSS=$(awk '/^VmHWM:/{print $2}' /proc/$P/status 2>/dev/null)
kill $P 2>/dev/null; wait $P 2>/dev/null
TP=$(grep throughput_msg_per_sec /tmp/c.json | grep -oE '[0-9]+' | tail -1)
P50=$(grep latency_p50_us /tmp/c.json | grep -oE '[0-9]+' | tail -1)
P99=$(grep latency_p99_us /tmp/c.json | grep -oE '[0-9]+' | tail -1)
echo "c10k/64K xylem: throughput=$TP  p50=$((${P50:-0}/1000))ms  p99=$((${P99:-0}/1000))ms  peak_rss=$(( ${RSS:-0}/1024 ))MB"
