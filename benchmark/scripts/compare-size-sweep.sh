#!/usr/bin/env bash
# Sweep payload size to distinguish scheduler vs bandwidth bottleneck.
set -uo pipefail

BIN="$(cd "$(dirname "$0")"/.. && pwd)/bin"
PORT=9900
DUR="${DUR:-6}"
CONNS="${CONNS:-1000}"
WORKERS="${WORKERS:-$(nproc)}"

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true

for SIZE in 64 1024 16384 65536; do
    echo "=== size=${SIZE}B  conns=${CONNS}  dur=${DUR}s ==="
    for label in xylem rust go; do
        case "$label" in
            xylem) srv="$BIN/tcp-xylem-echo-mt" ;;
            rust)  srv="$BIN/tcp-rust-echo-mt" ;;
            go)    srv="$BIN/tcp-go-echo-mt" ;;
        esac
        pkill -9 -f 'tcp-.*-echo' 2>/dev/null || true
        sleep 1
        "$srv" "$PORT" "$WORKERS" >/dev/null 2>&1 &
        SPID=$!
        sleep 2
        out="$("$BIN/tcp-bench" throughput -n "$CONNS" -d "$DUR" -s "$SIZE" -p "$PORT" 2>/dev/null)"
        tp="$( echo "$out" | grep throughput_msg_per_sec | grep -oE '[0-9]+' | tail -1 )"
        p50="$(echo "$out" | grep latency_p50_us         | grep -oE '[0-9]+' | tail -1 )"
        p99="$(echo "$out" | grep latency_p99_us         | grep -oE '[0-9]+' | tail -1 )"
        mb=0
        if [ -n "$tp" ]; then
            mb=$(( tp * SIZE / 1048576 ))
        fi
        printf '  %-6s  tp=%10s msg/s  %5s MB/s  p50=%7s us  p99=%9s us\n' \
            "$label" "${tp:-?}" "$mb" "${p50:-?}" "${p99:-?}"
        kill -TERM "$SPID" 2>/dev/null || true
        sleep 1
    done
    echo ""
done
