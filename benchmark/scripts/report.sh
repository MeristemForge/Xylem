#!/usr/bin/env bash
set -euo pipefail

# Generates a comparison table from benchmark JSON results.
# Usage: ./report.sh [results-dir]
#   If no dir given, uses the latest timestamped directory in benchmark/results/.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_DIR="$BENCH_DIR/results"

info() { printf "\033[1;34m[report]\033[0m %s\n" "$1"; }

# --- Find results directory --------------------------------------------------

if [ $# -ge 1 ]; then
    RUN_DIR="$1"
else
    RUN_DIR="$(ls -td "$RESULTS_DIR"/2* 2>/dev/null | head -1)"
    if [ -z "$RUN_DIR" ]; then
        echo "no results found in $RESULTS_DIR"
        exit 1
    fi
fi

info "results from: $RUN_DIR"
echo ""

# --- Parse and display -------------------------------------------------------

print_header() {
    printf "%-10s %-10s %-8s %12s %12s %12s %12s %10s\n" \
        "PROTOCOL" "SERVER" "SCALE" "CONNS" "MSG/SEC" "P50 (us)" "P99 (us)" "RSS (KB)"
    printf "%-10s %-10s %-8s %12s %12s %12s %12s %10s\n" \
        "--------" "--------" "------" "--------" "--------" "--------" "--------" "--------"
}

print_row() {
    local file="$1"
    local basename
    basename="$(basename "$file" .json)"

    # parse: proto-server-scale (e.g. tcp-xylem-1k) or proto-server (legacy)
    local proto server scale
    proto="${basename%%-*}"
    local rest="${basename#*-}"
    if [[ "$rest" =~ ^(.+)-([0-9]+k)$ ]]; then
        server="${BASH_REMATCH[1]}"
        scale="${BASH_REMATCH[2]}"
    else
        server="$rest"
        scale=""
    fi

    local conns throughput latency_p50 latency_p99 latency_max rss
    conns="$(jq -r '.connections // 0' "$file" 2>/dev/null)"
    throughput="$(jq -r '.throughput_msg_per_sec // 0' "$file" 2>/dev/null | cut -d. -f1)"
    latency_p50="$(jq -r '.latency_p50_us // 0' "$file" 2>/dev/null)"
    latency_p99="$(jq -r '.latency_p99_us // 0' "$file" 2>/dev/null)"
    latency_max="$(jq -r '.latency_max_us // 0' "$file" 2>/dev/null)"
    rss="$(jq -r '.memory_rss_kb // 0' "$file" 2>/dev/null)"

    local display_scale="${scale:-"-"}"
    printf "%-10s %-10s %-8s %12s %12s %12s %12s %10s\n" \
        "$proto" "$server" "$display_scale" "$conns" "$throughput" "$latency_p50" "$latency_p99" "$rss"
}

# --- Group by protocol -------------------------------------------------------

REPORT_FILE="$RUN_DIR/report.txt"

{
    print_header

    for proto in tcp udp tls dtls rudp; do
        files=("$RUN_DIR"/${proto}-*.json 2>/dev/null)
        if [ ! -f "${files[0]:-}" ]; then
            continue
        fi
        for f in "$RUN_DIR"/${proto}-*.json; do
            print_row "$f"
        done
    done
} | tee "$REPORT_FILE"

echo ""
info "report saved to $REPORT_FILE"

# --- Markdown version --------------------------------------------------------

MD_FILE="$RUN_DIR/report.md"

{
    echo "# Benchmark Results"
    echo ""
    echo "| Protocol | Server | Scale | Conns | Throughput (msg/s) | P50 (us) | P99 (us) | RSS (KB) |"
    echo "|----------|--------|------:|------:|-------------------:|----------:|----------:|---------:|"

    for proto in tcp udp tls dtls rudp; do
        for f in "$RUN_DIR"/${proto}-*.json 2>/dev/null; do
            [ -f "$f" ] || continue
            local_basename="$(basename "$f" .json)"
            local_proto="${local_basename%%-*}"
            local_rest="${local_basename#*-}"

            local_server="$local_rest"
            local_scale=""
            if [[ "$local_rest" =~ ^(.+)-([0-9]+k)$ ]]; then
                local_server="${BASH_REMATCH[1]}"
                local_scale="${BASH_REMATCH[2]}"
            fi

            conns="$(jq -r '.connections // 0' "$f" 2>/dev/null)"
            throughput="$(jq -r '.throughput_msg_per_sec // 0' "$f" 2>/dev/null | cut -d. -f1)"
            latency_p50="$(jq -r '.latency_p50_us // 0' "$f" 2>/dev/null)"
            latency_p99="$(jq -r '.latency_p99_us // 0' "$f" 2>/dev/null)"
            rss="$(jq -r '.memory_rss_kb // 0' "$f" 2>/dev/null)"

            echo "| $local_proto | $local_server | ${local_scale:--} | $conns | $throughput | $latency_p50 | $latency_p99 | $rss |"
        done
    done
} > "$MD_FILE"

info "markdown report saved to $MD_FILE"
