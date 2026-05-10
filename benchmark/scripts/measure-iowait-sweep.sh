#!/usr/bin/env bash
# Sweep payload size on the stats-instrumented xylem echo and print
# iowait wait/resume latencies per size.
set -uo pipefail

BIN="$(cd "$(dirname "$0")"/.. && pwd)/bin"
PORT=9700
DUR="${DUR:-6}"
CONNS="${CONNS:-1000}"
WORKERS="${WORKERS:-$(nproc)}"

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true

for SIZE in 64 1024 16384 65536; do
    pkill -9 -f 'tcp-xylem-echo' 2>/dev/null || true
    sleep 1

    : > /tmp/stats.stderr
    "$BIN/tcp-xylem-echo-mt-stats" "$PORT" "$WORKERS" 2>/tmp/stats.stderr &
    SPID=$!
    sleep 2

    out="$("$BIN/tcp-bench" throughput -n "$CONNS" -d "$DUR" -s "$SIZE" -p "$PORT" 2>/dev/null)"

    tp="$( echo "$out" | grep throughput_msg_per_sec | grep -oE '[0-9]+' | tail -1 )"
    p50="$(echo "$out" | grep latency_p50_us         | grep -oE '[0-9]+' | tail -1 )"
    p99="$(echo "$out" | grep latency_p99_us         | grep -oE '[0-9]+' | tail -1 )"

    kill -USR1 "$SPID" 2>/dev/null || true
    sleep 0.2
    kill -TERM "$SPID" 2>/dev/null || true
    for _ in 1 2 3 4 5; do
        kill -0 "$SPID" 2>/dev/null || break
        sleep 1
    done

    stats=$(grep '\[iowait-stats sig10' /tmp/stats.stderr | head -1)
    samples=$(echo "$stats" | grep -oE 'samples=[0-9]+' | cut -d= -f2)
    wait_ns=$(echo "$stats" | grep -oE 'avg_wait=[0-9]+' | cut -d= -f2)
    resume_ns=$(echo "$stats" | grep -oE 'avg_resume=[0-9]+' | cut -d= -f2)

    wait_us="?"
    resume_us="?"
    if [ -n "$wait_ns" ]; then
        wait_us=$(( wait_ns / 1000 ))
    fi
    if [ -n "$resume_ns" ]; then
        resume_us="$resume_ns"
    fi

    printf 'size=%5dB  tp=%7s msg/s  p50=%7s us  p99=%7s us  wait=%6s us  resume=%4s ns  samples=%s\n' \
        "$SIZE" "${tp:-?}" "${p50:-?}" "${p99:-?}" "$wait_us" "$resume_us" "${samples:-?}"
done
