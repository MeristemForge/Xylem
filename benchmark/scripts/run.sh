#!/usr/bin/env bash
set -euo pipefail

# Xylem TCP Benchmark Suite
# Runs all benchmark modes in sequence: throughput (c1k, c10k, c25k), connrate, memory, multi-threaded.
# No arguments needed. Just run: ./run.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="$BENCH_DIR/bin"
RESULTS_DIR="$BENCH_DIR/results"

DURATION=10
WARMUP=3
CONCURRENCY=512

# --- Output helpers ----------------------------------------------------------

info() { printf "\033[1;34m[bench]\033[0m %s\n" "$1"; }
ok()   { printf "\033[1;32m[ok]\033[0m %s\n" "$1"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$1"; }

# --- Prerequisite check ------------------------------------------------------

if [ ! -d "$BIN_DIR" ]; then
    warn "bin/ not found, running install-deps.sh && build.sh..."
    "$SCRIPT_DIR/install-deps.sh"
    "$SCRIPT_DIR/build.sh"
fi

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true

# --- Results directory -------------------------------------------------------

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$RESULTS_DIR/$TIMESTAMP"
mkdir -p "$RUN_DIR"

# --- Server registry ---------------------------------------------------------

SERVERS=(xylem libuv libevent libhv boost go rust)
PORT_BASE=9000

# --- Helpers -----------------------------------------------------------------

format_conns() {
    local c="$1"
    if [ "$c" -ge 1000 ]; then
        echo "$((c / 1000))k"
    else
        echo "$c"
    fi
}

start_server() {
    local bin="$1" port="$2" extra="${3:-}"
    if [ -n "$extra" ]; then
        "$bin" "$port" $extra &
    else
        "$bin" "$port" &
    fi
    SERVER_PID=$!
}

stop_server() {
    kill "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
}

get_rss() {
    if [ -f "/proc/$1/status" ]; then
        grep VmRSS "/proc/$1/status" 2>/dev/null | awk '{print $2}' || echo 0
    else
        echo 0
    fi
}

# =============================================================================
# 1. THROUGHPUT (echo ping-pong)
# =============================================================================

run_throughput() {
    local conns="$1"
    local label
    label="$(format_conns "$conns")"

    local warmup="$WARMUP"
    [ "$conns" -ge 10000 ] && warmup=5
    [ "$conns" -ge 50000 ] && warmup=10

    info "=== Throughput: c${label}, ${DURATION}s ==="

    local offset=0
    for name in "${SERVERS[@]}"; do
        local port=$((PORT_BASE + offset))
        local bin="$BIN_DIR/tcp-${name}-echo"
        offset=$((offset + 1))

        if [ ! -f "$bin" ]; then
            warn "skip $name (not found)"
            continue
        fi

        start_server "$bin" "$port"
        local pid=$SERVER_PID
        sleep "$warmup"

        local out="$RUN_DIR/throughput-${name}-${label}.json"
        "$BIN_DIR/tcp-bench" throughput -n "$conns" -d "$DURATION" -p "$port" > "$out" 2>/dev/null || true

        stop_server "$pid"
        sleep 1

        if [ -s "$out" ]; then
            local tp
            tp=$(grep throughput_msg_per_sec "$out" | grep -o '[0-9]*')
            ok "$name c${label}: ${tp} msg/s"
        else
            warn "$name c${label}: no results"
        fi
    done
    echo ""
}

# =============================================================================
# 2. CONNECTION RATE
# =============================================================================

run_connrate() {
    info "=== Connection Rate: concurrency=${CONCURRENCY}, ${DURATION}s ==="

    local offset=0
    for name in "${SERVERS[@]}"; do
        local port=$((PORT_BASE + offset))
        local bin="$BIN_DIR/tcp-${name}-echo"
        offset=$((offset + 1))

        if [ ! -f "$bin" ]; then
            warn "skip $name (not found)"
            continue
        fi

        start_server "$bin" "$port"
        local pid=$SERVER_PID
        sleep 2

        local out="$RUN_DIR/connrate-${name}.json"
        "$BIN_DIR/tcp-bench" connrate -c "$CONCURRENCY" -d "$DURATION" -p "$port" > "$out" 2>/dev/null || true

        stop_server "$pid"
        sleep 1

        if [ -s "$out" ]; then
            local cps
            cps=$(grep connects_per_sec "$out" | grep -o '[0-9]*')
            ok "$name: ${cps} conn/s"
        else
            warn "$name: no results"
        fi
    done
    echo ""
}

# =============================================================================
# 3. MEMORY
# =============================================================================

run_memory() {
    local conns="$1"
    local label
    label="$(format_conns "$conns")"

    info "=== Memory: c${label} ==="

    local offset=0
    for name in "${SERVERS[@]}"; do
        local port=$((PORT_BASE + offset))
        local bin="$BIN_DIR/tcp-${name}-echo"
        offset=$((offset + 1))

        if [ ! -f "$bin" ]; then
            warn "skip $name (not found)"
            continue
        fi

        start_server "$bin" "$port"
        local pid=$SERVER_PID
        sleep 2

        "$BIN_DIR/tcp-bench" memory -n "$conns" -w 5 -p "$port" > /dev/null 2>/dev/null &
        local cpid=$!
        sleep 8

        local rss
        rss=$(get_rss "$pid")

        wait "$cpid" 2>/dev/null || true

        local out="$RUN_DIR/memory-${name}-${label}.json"
        printf '{"server":"%s","connections":%d,"server_rss_kb":%s}\n' \
            "$name" "$conns" "$rss" > "$out"

        ok "$name c${label}: ${rss} KB"

        stop_server "$pid"
        sleep 1
    done
    echo ""
}

# =============================================================================
# 4. PAYLOAD SIZE (throughput at different message sizes, c1k)
# =============================================================================

run_payload() {
    local sizes=(64 1024 4096 65536)

    for size in "${sizes[@]}"; do
        local size_label="${size}B"
        [ "$size" -ge 1024 ] && size_label="$((size / 1024))KB"

        info "=== Payload: ${size_label}, c1k, ${DURATION}s ==="

        local offset=0
        for name in "${SERVERS[@]}"; do
            local port=$((PORT_BASE + offset))
            local bin="$BIN_DIR/tcp-${name}-echo"
            offset=$((offset + 1))

            if [ ! -f "$bin" ]; then
                warn "skip $name (not found)"
                continue
            fi

            start_server "$bin" "$port"
            local pid=$SERVER_PID
            sleep "$WARMUP"

            local out="$RUN_DIR/payload-${name}-${size_label}.json"
            "$BIN_DIR/tcp-bench" throughput -n 1000 -d "$DURATION" -s "$size" -p "$port" > "$out" 2>/dev/null || true

            stop_server "$pid"
            sleep 1

            if [ -s "$out" ]; then
                local tp
                tp=$(grep throughput_msg_per_sec "$out" | grep -o '[0-9]*')
                local mbps=$(( tp * size / 1048576 ))
                ok "$name ${size_label}: ${tp} msg/s (${mbps} MB/s)"
            else
                warn "$name ${size_label}: no results"
            fi
        done
        echo ""
    done
}

# =============================================================================
# MAIN — run everything
# =============================================================================

info "Xylem TCP Benchmark Suite"
info "results -> $RUN_DIR"
echo ""

# Throughput scaling curve
run_throughput 100
run_throughput 500
run_throughput 1000
run_throughput 5000
run_throughput 10000

# Connection rate
run_connrate

# Memory at 10k connections
run_memory 10000

# Payload sizes
run_payload

# Summary
echo ""
ok "ALL BENCHMARKS COMPLETE"
info "results in $RUN_DIR"
echo ""
ls -1 "$RUN_DIR/"
