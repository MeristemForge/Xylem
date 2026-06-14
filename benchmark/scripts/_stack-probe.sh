#!/usr/bin/env bash
# Probe c10k/64K TLS: vary coroutine stack size, measure throughput + peak RSS.
set -u
cd "$(dirname "$0")/../out" || exit 1
ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true

PORT=9543
CONNS=10000
DUR=10
PAYLOAD=65536

printf "%-10s %12s %12s %10s\n" "coro_stack" "throughput" "peak_rss_MB" "p50_ms"
for S in 131072 65536 32768 16384 8192; do
    XYLEM_CORO_STACK=$S taskset -c 0 ./tls-xylem-echo $PORT >/dev/null 2>&1 &
    PID=$!
    sleep 2
    GOMAXPROCS=8 taskset -c 8-15 ./tls-bench throughput \
        -n $CONNS -d $DUR -s $PAYLOAD -p $PORT > /tmp/sp-$S.json 2>/dev/null
    RSS=$(awk '/^VmHWM:/{print $2}' /proc/$PID/status 2>/dev/null)
    TP=$(grep throughput_msg_per_sec /tmp/sp-$S.json | grep -oE '[0-9]+' | tail -1)
    P50=$(grep latency_p50_us /tmp/sp-$S.json | grep -oE '[0-9]+' | tail -1)
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    sleep 1
    printf "%-10s %12s %12s %10s\n" \
        "$S" "${TP:-NA}" "$(( ${RSS:-0} / 1024 ))" "$(( ${P50:-0} / 1000 ))"
done
