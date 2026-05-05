#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/../bin" && pwd)"
PORT=9300
DUR=10
CONNS=1000

extract() {
    local key="$1" json="$2"
    echo "$json" | sed -n "s/.*\"${key}\": *\([0-9]*\).*/\1/p"
}

run_one() {
    local bin="$1" port="$2" workers="${3:-}" conns="${4:-$CONNS}" dur="${5:-$DUR}"
    if [ -n "$workers" ]; then
        "$bin" "$port" "$workers" >/dev/null 2>&1 &
    else
        "$bin" "$port" >/dev/null 2>&1 &
    fi
    local pid=$!
    sleep 3
    local result
    result=$("$BIN/tcp-bench-win.exe" throughput -n "$conns" -d "$dur" -p "$port" 2>/dev/null || true)
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    sleep 1
    echo "$result"
}

print_result() {
    local name="$1" result="$2"
    if [ -n "$result" ]; then
        local tp p50 p99 pmax
        tp=$(extract "throughput_msg_per_sec" "$result")
        p50=$(extract "latency_p50_us" "$result")
        p99=$(extract "latency_p99_us" "$result")
        pmax=$(extract "latency_max_us" "$result")
        printf "%-12s %10s %10s %10s %10s\n" "$name" "$tp" "$p50" "$p99" "$pmax"
    else
        printf "%-12s FAILED\n" "$name"
    fi
}

echo "============================================================"
echo "  Windows TCP Benchmark — $(date +%Y-%m-%d\ %H:%M)"
echo "  Payload: 64B, Duration: ${DUR}s"
echo "============================================================"
echo ""

# ============================================================
# Part 1: Single-worker throughput (all frameworks)
# ============================================================
echo "============================================================"
echo "  PART 1: Single Worker — Throughput (${CONNS} conns)"
echo "============================================================"
printf "%-12s %10s %10s %10s %10s\n" "Server" "msg/s" "p50(us)" "p99(us)" "max(us)"
printf "%-12s %10s %10s %10s %10s\n" "----------" "--------" "--------" "--------" "--------"

SINGLE_SERVERS="xylem-echo asio-echo libuv-echo libevent-echo go-echo-mt rust-echo-mt"
SINGLE_NAMES="xylem asio libuv libevent go rust"

i=0
for srv in $SINGLE_SERVERS; do
    name=$(echo "$SINGLE_NAMES" | cut -d' ' -f$((i+1)))
    bin="$BIN/tcp-${srv}.exe"
    if [ ! -f "$bin" ]; then
        printf "%-12s SKIP (not found)\n" "$name"
        i=$((i+1))
        continue
    fi
    # For go/rust mt servers, pass workers=1
    if [[ "$srv" == *-mt ]]; then
        result=$(run_one "$bin" $PORT "1")
    else
        result=$(run_one "$bin" $PORT "")
    fi
    print_result "$name" "$result"
    i=$((i+1))
done
echo ""

# Also run xylem-echo-mt with 1 worker for fair comparison
bin="$BIN/tcp-xylem-echo-mt.exe"
if [ -f "$bin" ]; then
    result=$(run_one "$bin" $PORT "1")
    print_result "xylem(mt=1)" "$result"
fi
echo ""

# ============================================================
# Part 2: Multi-worker scaling (Xylem, Go, Rust)
# ============================================================
echo "============================================================"
echo "  PART 2: Multi-Worker Scaling (${CONNS} conns)"
echo "============================================================"
echo ""

for workers in 4 8 16; do
    echo "--- Workers: ${workers} ---"
    printf "%-12s %10s %10s %10s %10s\n" "Server" "msg/s" "p50(us)" "p99(us)" "max(us)"
    printf "%-12s %10s %10s %10s %10s\n" "----------" "--------" "--------" "--------" "--------"

    for name in xylem go rust; do
        bin="$BIN/tcp-${name}-echo-mt.exe"
        if [ ! -f "$bin" ]; then
            printf "%-12s SKIP\n" "$name"
            continue
        fi
        result=$(run_one "$bin" $PORT "$workers")
        print_result "$name" "$result"
    done
    echo ""
done

echo "============================================================"
echo "  Done"
echo "============================================================"
