#!/usr/bin/env bash
set -uo pipefail

BIN="$(cd "$(dirname "$0")"/.. && pwd)/bin"
PORT=9700
DUR="${DUR:-6}"
SIZE="${SIZE:-64}"
WORKERS="${WORKERS:-$(nproc)}"

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true

for LABEL in xylem rust; do
    case "$LABEL" in
        xylem) srv="$BIN/tcp-xylem-echo-mt" ;;
        rust)  srv="$BIN/tcp-rust-echo-mt" ;;
    esac

    for CONNS in 16 64 256 1024 4096; do
        pkill -9 -f 'tcp-.*-echo' 2>/dev/null || true
        sleep 1
        "$srv" "$PORT" "$WORKERS" >/dev/null 2>&1 &
        SPID=$!
        sleep 2
        out="$("$BIN/tcp-bench" throughput -n "$CONNS" -d "$DUR" -s "$SIZE" -p "$PORT" 2>/dev/null)"
        tp="$( echo "$out" | grep throughput_msg_per_sec | grep -oE '[0-9]+' | tail -1 )"
        p50="$(echo "$out" | grep latency_p50_us         | grep -oE '[0-9]+' | tail -1 )"
        p99="$(echo "$out" | grep latency_p99_us         | grep -oE '[0-9]+' | tail -1 )"
        printf '%-6s conns=%5d  tp=%8s msg/s  p50=%7s us  p99=%7s us\n' \
            "$LABEL" "$CONNS" "${tp:-?}" "${p50:-?}" "${p99:-?}"
        kill -TERM "$SPID" 2>/dev/null || true
        sleep 1
    done
    echo
done
