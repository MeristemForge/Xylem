#!/usr/bin/env bash
set -euo pipefail

# Benchmark xylem ET mode with different worker counts (1, 4, 8, 16)
# Measures throughput and connection rate

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
BIN="$BENCH_DIR/bin"
BUILD="$BENCH_DIR/build"
PORT=9000
DUR=15
CONNS=1000

ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true
pkill -f "tcp-xylem" 2>/dev/null || true
sleep 1

# --- Rebuild xylem library ---
echo "=== Rebuilding xylem library (Release) ==="
cmake -S "$PROJECT_ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-O3 -DNDEBUG -flto" \
    -DXYLEM_ENABLE_TLS=OFF \
    -G Ninja >/dev/null 2>&1
ninja -C "$BUILD" xylem -j"$(nproc)"
echo "OK: libxylem.a rebuilt"

# --- Rebuild xylem echo-mt ---
echo "=== Rebuilding tcp-xylem-echo-mt ==="
gcc -O3 -DNDEBUG -flto \
    -I"$PROJECT_ROOT/include" \
    "$BENCH_DIR/tcp/server/xylem-echo-mt.c" \
    "$BUILD/libxylem.a" \
    -lpthread -lssl -lcrypto \
    -s -flto \
    -o "$BIN/tcp-xylem-echo-mt"
echo "OK: tcp-xylem-echo-mt rebuilt"
echo ""

# --- Run benchmarks ---
echo "=== TCP ET-Mode Benchmark: ${DUR}s, 64B payload, ${CONNS} connections ==="
echo ""

printf "%-8s %12s %12s %12s %12s\n" "Workers" "Throughput" "P50(us)" "P99(us)" "ConnRate"
printf "%-8s %12s %12s %12s %12s\n" "-------" "----------" "-------" "-------" "--------"

for workers in 1 4 8 16; do
    pkill -f "tcp-xylem" 2>/dev/null || true
    sleep 1

    "$BIN/tcp-xylem-echo-mt" $PORT $workers >/dev/null 2>&1 &
    pid=$!
    sleep 2

    # Throughput test
    tp_result=$("$BIN/tcp-bench" throughput -n $CONNS -d $DUR -p $PORT 2>/dev/null || true)
    tp=""
    p50=""
    p99=""
    if [ -n "$tp_result" ]; then
        tp=$(echo "$tp_result" | grep throughput_msg_per_sec | grep -o "[0-9]*")
        p50=$(echo "$tp_result" | grep latency_p50_us | grep -o "[0-9]*")
        p99=$(echo "$tp_result" | grep latency_p99_us | grep -o "[0-9]*")
    fi

    sleep 1

    # Connection rate test
    cr_result=$("$BIN/tcp-bench" connrate -c 512 -d $DUR -p $PORT 2>/dev/null || true)
    cps=""
    if [ -n "$cr_result" ]; then
        cps=$(echo "$cr_result" | grep connects_per_sec | grep -o "[0-9]*")
    fi

    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true

    printf "%-8s %12s %12s %12s %12s\n" \
        "$workers" \
        "${tp:-FAIL}" \
        "${p50:-FAIL}" \
        "${p99:-FAIL}" \
        "${cps:-FAIL}"
done

echo ""
echo "=== Done ==="
