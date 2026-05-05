#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/../bin" && pwd)"
PORT=9000
DUR=10
WARMUP=3

pkill -f "tcp-.*-echo" 2>/dev/null || true
sleep 1
ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true

extract() {
    local key="$1" json="$2"
    echo "$json" | sed -n "s/.*\"${key}\": *\([0-9]*\).*/\1/p"
}

run_bench() {
    local bin="$1" port="$2" workers="${3:-}" bench_args="$4" warmup="${5:-$WARMUP}"
    if [ -n "$workers" ]; then
        "$bin" "$port" "$workers" >/dev/null 2>&1 &
    else
        "$bin" "$port" >/dev/null 2>&1 &
    fi
    local pid=$!
    sleep "$warmup"
    local result
    result=$("$BIN/tcp-bench" $bench_args -p "$port" 2>/dev/null || true)
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    sleep 1
    echo "$result"
}

print_tp() {
    local name="$1" result="$2"
    if [ -n "$result" ]; then
        local tp p50 p99 pmax
        tp=$(extract "throughput_msg_per_sec" "$result")
        p50=$(extract "latency_p50_us" "$result")
        p99=$(extract "latency_p99_us" "$result")
        pmax=$(extract "latency_max_us" "$result")
        printf "  %-10s %10s msg/s   p50=%6s us  p99=%6s us  max=%6s us\n" "$name" "$tp" "$p50" "$p99" "$pmax"
    else
        printf "  %-10s FAILED\n" "$name"
    fi
}

print_cr() {
    local name="$1" result="$2"
    if [ -n "$result" ]; then
        local cps ok fail
        cps=$(extract "connects_per_sec" "$result")
        ok=$(extract "total_connects" "$result")
        fail=$(extract "failed_connects" "$result")
        printf "  %-10s %10s conn/s   (ok=%s fail=%s)\n" "$name" "$cps" "$ok" "$fail"
    else
        printf "  %-10s FAILED\n" "$name"
    fi
}

echo "============================================================"
echo "  WSL TCP Benchmark — Go vs Xylem vs Rust"
echo "  $(date +%Y-%m-%d\ %H:%M)  Duration: ${DUR}s per test"
echo "============================================================"
echo ""

# ==============================================================================
# PART 1: Single Worker Throughput (payload=64B, varying connections)
# ==============================================================================
echo "============================================================"
echo "  PART 1: Single Worker — Throughput (payload=64B)"
echo "============================================================"
echo ""

for conns in 100 500 1000 5000 10000; do
    label="$conns"
    [ "$conns" -ge 1000 ] && label="$((conns/1000))k"
    warmup=$WARMUP
    [ "$conns" -ge 10000 ] && warmup=5

    echo "--- c${label} connections ---"
    for name in xylem go rust; do
        bin="$BIN/tcp-${name}-echo"
        if [ ! -f "$bin" ]; then printf "  %-10s SKIP\n" "$name"; continue; fi
        result=$(run_bench "$bin" $PORT "" "throughput -n $conns -d $DUR" "$warmup")
        print_tp "$name" "$result"
    done
    echo ""
done

# ==============================================================================
# PART 2: Single Worker — Varying Payload (conns=1000)
# ==============================================================================
echo "============================================================"
echo "  PART 2: Single Worker — Varying Payload (c1000)"
echo "============================================================"
echo ""

for size in 64 512 4096 65536; do
    size_label="${size}B"
    [ "$size" -ge 1024 ] && size_label="$((size/1024))KB"

    echo "--- payload=${size_label} ---"
    for name in xylem go rust; do
        bin="$BIN/tcp-${name}-echo"
        if [ ! -f "$bin" ]; then printf "  %-10s SKIP\n" "$name"; continue; fi
        result=$(run_bench "$bin" $PORT "" "throughput -n 1000 -d $DUR -s $size" "$WARMUP")
        print_tp "$name" "$result"
    done
    echo ""
done

# ==============================================================================
# PART 3: Multi-Worker Throughput (payload=64B)
# ==============================================================================
echo "============================================================"
echo "  PART 3: Multi-Worker — Throughput (payload=64B)"
echo "============================================================"
echo ""

for workers in 4 8 16; do
    for conns in 1000 10000; do
        label="$conns"
        [ "$conns" -ge 1000 ] && label="$((conns/1000))k"
        warmup=$WARMUP
        [ "$conns" -ge 10000 ] && warmup=5

        echo "--- ${workers} workers, c${label} ---"
        for name in xylem go rust; do
            bin="$BIN/tcp-${name}-echo-mt"
            if [ ! -f "$bin" ]; then printf "  %-10s SKIP\n" "$name"; continue; fi
            result=$(run_bench "$bin" $PORT "$workers" "throughput -n $conns -d $DUR" "$warmup")
            print_tp "$name" "$result"
        done
        echo ""
    done
done

# ==============================================================================
# PART 4: Connection Rate (connrate)
# ==============================================================================
echo "============================================================"
echo "  PART 4: Connection Rate (concurrency=512)"
echo "============================================================"
echo ""

echo "--- Single Worker ---"
for name in xylem go rust; do
    bin="$BIN/tcp-${name}-echo"
    if [ ! -f "$bin" ]; then printf "  %-10s SKIP\n" "$name"; continue; fi
    result=$(run_bench "$bin" $PORT "" "connrate -c 512 -d $DUR" "3")
    print_cr "$name" "$result"
done
echo ""

echo "--- 8 Workers ---"
for name in xylem go rust; do
    bin="$BIN/tcp-${name}-echo-mt"
    if [ ! -f "$bin" ]; then printf "  %-10s SKIP\n" "$name"; continue; fi
    result=$(run_bench "$bin" $PORT "8" "connrate -c 512 -d $DUR" "3")
    print_cr "$name" "$result"
done
echo ""

echo "============================================================"
echo "  Done"
echo "============================================================"
