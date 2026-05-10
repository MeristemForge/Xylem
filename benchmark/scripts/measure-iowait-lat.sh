#!/usr/bin/env bash
# Run the stats-instrumented xylem echo server and print per-park
# wait/resume latency averages for a given workload.
set -uo pipefail

BIN="$(cd "$(dirname "$0")"/.. && pwd)/bin"
PORT=9700
DUR="${DUR:-6}"
CONNS="${CONNS:-1000}"
SIZE="${SIZE:-64}"
WORKERS="${WORKERS:-$(nproc)}"

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true
pkill -9 -f 'tcp-xylem-echo' 2>/dev/null || true
sleep 1

"$BIN/tcp-xylem-echo-mt-stats" "$PORT" "$WORKERS" 2>/tmp/stats.stderr &
SPID=$!
sleep 2

out="$("$BIN/tcp-bench" throughput -n "$CONNS" -d "$DUR" -s "$SIZE" -p "$PORT" 2>/dev/null)"

tp="$( echo "$out" | grep throughput_msg_per_sec | grep -oE '[0-9]+' | tail -1 )"
p50="$(echo "$out" | grep latency_p50_us         | grep -oE '[0-9]+' | tail -1 )"
p99="$(echo "$out" | grep latency_p99_us         | grep -oE '[0-9]+' | tail -1 )"

# Signal for a live snapshot, then terminate for final.
kill -USR1 "$SPID" 2>/dev/null || true
sleep 0.2
kill -TERM "$SPID" 2>/dev/null || true
for _ in 1 2 3 4 5; do
    kill -0 "$SPID" 2>/dev/null || break
    sleep 1
done

echo "conns=$CONNS size=${SIZE}B dur=${DUR}s workers=$WORKERS"
printf "throughput=%s msg/s  p50=%s us  p99=%s us\n" "${tp:-?}" "${p50:-?}" "${p99:-?}"
echo "--- stderr from server ---"
grep -E '\[(iowait|sched)-stats' /tmp/stats.stderr || echo "(no stats printed)"
