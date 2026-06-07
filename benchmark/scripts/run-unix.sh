#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Xylem TCP benchmark suite (cross-platform: Linux + macOS)
# -----------------------------------------------------------------------------
#   install  - install system dependencies (Linux: sudo apt + source builds;
#              macOS: guidance via brew)
#   build    - build xylem + all TCP echo servers (ST + MT) + bench client
#   bench    - run comparison benchmarks and write results/<ts>/
#   all      - install + build + bench                             [default]
#
# Compared servers (Linux: 5 families): xylem, libuv, boost, go, rust
# On macOS the default set narrows to xylem, go, rust (libuv/boost are
# typically absent; missing binaries are skipped automatically anyway).
# Each family has a single-threaded (ST) and multi-threaded (MT) binary.
#
# NOTE: macOS uses kqueue and lacks SO_REUSEPORT / /proc; its numbers are
# NOT comparable to the Linux suite. Per-CPU usage sampling is Linux-only.
#
# Fairness rules for MT servers:
#   - MT workers run as N pthreads / N goroutines / N tokio workers.
#   - Each worker has its own listen socket with SO_REUSEPORT so the
#     kernel load-balances accepts (except xylem/go/rust which use
#     their native shared-runtime work-stealing -- each project's
#     idiomatic MT story).
#   - TCP_NODELAY on accepted sockets, backlog 4096, 64 KB read buffer.
#
# Benchmark matrix (defaults, configurable via CLI):
#   ST row : payload in {64B, 4KB, 64KB} x conns in {1k, 10k}  = 6 runs / family
#   MT row : same matrix with workers = $(nproc)                = 6 runs / family
#   ConnRate : concurrency in {1k, 10k}                         = 2 runs x 2 rows
#
# CLI options for `bench`:
#   --servers xylem,go,rust   select which servers to compare (comma-separated)
#   --conns 1000,10000        connection counts (comma-separated)
#   --payload 64,4096         payload sizes in bytes (comma-separated)
#   --duration 10             test duration in seconds
#   --mode st|mt|both         single-thread, multi-thread, or both (default: both)
#   --no-connrate             skip connection-rate tests
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
BIN_DIR="$BENCH_DIR/bin"
BUILD_DIR="$BENCH_DIR/build"
RESULTS_ROOT="$BENCH_DIR/results"

info() { printf "\033[1;34m[bench]\033[0m %s\n" "$1"; }
ok()   { printf "\033[1;32m[ok]\033[0m %s\n" "$1"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$1"; }
err()  { printf "\033[1;31m[err]\033[0m %s\n" "$1" >&2; }

# =============================================================================
# platform detection
# =============================================================================

if [ "$(uname -s)" = "Darwin" ]; then
    PLATFORM="macos"
    CPU_SAMPLING=false          # no /proc/stat on macOS
    ULIMIT_HARD=100000
    ncpu() { sysctl -n hw.ncpu; }
else
    PLATFORM="linux"
    CPU_SAMPLING=true
    ULIMIT_HARD=200000
    ncpu() { nproc; }
fi

# =============================================================================
# install
# =============================================================================

cmd_install() {
    if [ "$PLATFORM" = "macos" ]; then
        info "macOS detected; installing deps via Homebrew..."
        if ! command -v brew >/dev/null 2>&1; then
            err "Homebrew not found. Install it from https://brew.sh then re-run."
            exit 1
        fi
        local BREW_PKGS=(cmake ninja pkg-config openssl go rust libuv boost)
        local missing=()
        local pkg
        for pkg in "${BREW_PKGS[@]}"; do
            brew list "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
        done
        if [ "${#missing[@]}" -gt 0 ]; then
            brew install "${missing[@]}"
        fi
        ok "all dependencies ready (macOS)"
        return
    fi

    if [ "$(id -u)" -ne 0 ]; then
        info "escalating to root for dependency install..."
        exec sudo -E "$0" install
    fi

    local LIBUV_VERSION="1.49.2"
    local BOOST_VERSION="1.87.0"
    local PREFIX="/usr/local"
    local JOBS; JOBS="$(ncpu)"

    info "apt base packages..."
    local APT_PKGS=(
        build-essential cmake ninja-build pkg-config
        autoconf automake libtool
        libssl-dev
        golang-go
        curl git
    )
    local missing=()
    for pkg in "${APT_PKGS[@]}"; do
        dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        apt-get update -qq
        apt-get install -y -qq "${missing[@]}"
    fi
    ok "apt packages ready"

    info "rust..."
    if ! command -v cargo >/dev/null 2>&1; then
        # shellcheck disable=SC2016
        sudo -u "${SUDO_USER:-$USER}" bash -c '
            curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs |
                sh -s -- -y --quiet
        '
    fi
    ok "rust ready"

    install_from_source() {
        local name="$1" url="$2" dir="$3" marker="$4"
        shift 4
        if [ -f "$marker" ]; then
            ok "$name already installed"
            return
        fi
        info "building $name from source..."
        local tmp; tmp="$(mktemp -d)"
        ( cd "$tmp"
          curl -sSL "$url" | tar xz
          cd "$dir"
          mkdir -p build && cd build
          cmake .. "$@" \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_C_FLAGS="-O3 -DNDEBUG -flto" \
              -DCMAKE_INSTALL_PREFIX="$PREFIX"
          make -j"$JOBS"
          make install
        )
        ldconfig
        rm -rf "$tmp"
        ok "$name installed"
    }

    install_from_source libuv \
        "https://github.com/libuv/libuv/archive/refs/tags/v${LIBUV_VERSION}.tar.gz" \
        "libuv-${LIBUV_VERSION}" \
        "$PREFIX/lib/libuv.a" \
        -DBUILD_TESTING=OFF -DLIBUV_BUILD_SHARED=OFF

    if [ ! -f "$PREFIX/lib/libboost_system.a" ]; then
        info "building boost v${BOOST_VERSION} from source..."
        local tmp; tmp="$(mktemp -d)"
        local boost_underscore="${BOOST_VERSION//./_}"
        ( cd "$tmp"
          curl -sSL "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${boost_underscore}.tar.gz" | tar xz
          cd "boost_${boost_underscore}"
          ./bootstrap.sh --prefix="$PREFIX" --with-libraries=system
          ./b2 install \
              variant=release \
              optimization=speed \
              link=static \
              cxxflags="-O3 -DNDEBUG -flto" \
              linkflags="-flto" \
              -j"$JOBS" \
              --prefix="$PREFIX" 2>/dev/null
        )
        ldconfig
        rm -rf "$tmp"
        ok "boost ${BOOST_VERSION} installed"
    else
        ok "boost already installed"
    fi

    ok "all dependencies ready"
}

# =============================================================================
# build
# =============================================================================

cmd_build() {
    mkdir -p "$BIN_DIR"

    local CFLAGS_COMMON="-O3 -DNDEBUG -flto -Wall -Wextra"
    local LDFLAGS_COMMON="-s -flto"

    info "building xylem static library..."
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS='-O3 -DNDEBUG -flto' \
        -DXYLEM_ENABLE_TLS=OFF \
        -G Ninja >/dev/null 2>&1
    ninja -C "$BUILD_DIR" xylem -j"$(ncpu)"
    local XYLEM_LIB="$BUILD_DIR/libxylem.a"
    ok "xylem built"

    info "building tcp servers (ST)..."
    # shellcheck disable=SC2086
    gcc $CFLAGS_COMMON -I"$PROJECT_ROOT/include" \
        "$BENCH_DIR/tcp/server/xylem-echo.c" \
        "$XYLEM_LIB" -lpthread $LDFLAGS_COMMON \
        -o "$BIN_DIR/tcp-xylem-echo"

    local cand
    for cand in libuv; do
        local src="$BENCH_DIR/tcp/server/${cand}-echo.c"
        [ -f "$src" ] || continue
        local lflag="-l${cand#lib}"
        # shellcheck disable=SC2086
        gcc $CFLAGS_COMMON "$src" $lflag -lpthread $LDFLAGS_COMMON \
            -o "$BIN_DIR/tcp-${cand}-echo" || warn "skip ${cand} ST (build failed)"
    done

    if [ -f "$BENCH_DIR/tcp/server/boost-echo.cpp" ]; then
        # shellcheck disable=SC2086
        g++ $CFLAGS_COMMON -std=c++17 \
            "$BENCH_DIR/tcp/server/boost-echo.cpp" \
            -lboost_system -lpthread $LDFLAGS_COMMON \
            -o "$BIN_DIR/tcp-boost-echo" || warn "skip boost ST (build failed)"
    fi

    if [ -d "$BENCH_DIR/tcp/server/go-echo" ] && command -v go >/dev/null 2>&1; then
        ( cd "$BENCH_DIR/tcp/server/go-echo" && \
          CGO_ENABLED=0 go build -ldflags="-s -w" -o "$BIN_DIR/tcp-go-echo" . ) \
          || warn "skip go ST (build failed)"
    fi

    if [ -d "$BENCH_DIR/tcp/server/rust-echo" ] && command -v cargo >/dev/null 2>&1; then
        ( cd "$BENCH_DIR/tcp/server/rust-echo" && cargo build --release -q --bin tcp-rust-echo && \
          cp target/release/tcp-rust-echo "$BIN_DIR/" && \
          strip "$BIN_DIR/tcp-rust-echo" ) \
          || warn "skip rust ST (build failed)"
    fi
    ok "tcp servers (ST) built"

    info "building tcp servers (MT)..."
    # shellcheck disable=SC2086
    gcc $CFLAGS_COMMON -I"$PROJECT_ROOT/include" \
        "$BENCH_DIR/tcp/server/xylem-echo-mt.c" \
        "$XYLEM_LIB" -lpthread $LDFLAGS_COMMON \
        -o "$BIN_DIR/tcp-xylem-echo-mt"

    for cand in libuv; do
        local src="$BENCH_DIR/tcp/server/${cand}-echo-mt.c"
        [ -f "$src" ] || continue
        local lflag="-l${cand#lib}"
        # shellcheck disable=SC2086
        gcc $CFLAGS_COMMON "$src" $lflag -lpthread $LDFLAGS_COMMON \
            -o "$BIN_DIR/tcp-${cand}-echo-mt" || warn "skip ${cand} MT (build failed)"
    done

    if [ -f "$BENCH_DIR/tcp/server/boost-echo-mt.cpp" ]; then
        # shellcheck disable=SC2086
        g++ $CFLAGS_COMMON -std=c++17 \
            "$BENCH_DIR/tcp/server/boost-echo-mt.cpp" \
            -lboost_system -lpthread $LDFLAGS_COMMON \
            -o "$BIN_DIR/tcp-boost-echo-mt" || warn "skip boost MT (build failed)"
    fi

    if [ -d "$BENCH_DIR/tcp/server/go-echo-mt" ] && command -v go >/dev/null 2>&1; then
        ( cd "$BENCH_DIR/tcp/server/go-echo-mt" && \
          CGO_ENABLED=0 go build -ldflags="-s -w" -o "$BIN_DIR/tcp-go-echo-mt" . ) \
          || warn "skip go MT (build failed)"
    fi

    if [ -d "$BENCH_DIR/tcp/server/rust-echo" ] && command -v cargo >/dev/null 2>&1; then
        ( cd "$BENCH_DIR/tcp/server/rust-echo" && cargo build --release -q --bin tcp-rust-echo-mt && \
          cp target/release/tcp-rust-echo-mt "$BIN_DIR/" && \
          strip "$BIN_DIR/tcp-rust-echo-mt" ) \
          || warn "skip rust MT (build failed)"
    fi
    ok "tcp servers (MT) built"

    info "building tcp-bench client..."
    # shellcheck disable=SC2086
    gcc $CFLAGS_COMMON \
        "$BENCH_DIR/tcp/client/tcp-bench.c" \
        $LDFLAGS_COMMON -o "$BIN_DIR/tcp-bench"
    ok "tcp-bench built"

    echo ""
    ls -lh "$BIN_DIR"
}

# =============================================================================
# bench
# =============================================================================

DURATION="${DURATION:-10}"
PORT_BASE="${PORT_BASE:-9000}"

ensure_bin() {
    if [ ! -d "$BIN_DIR" ] || [ ! -x "$BIN_DIR/tcp-bench" ]; then
        err "binaries missing in $BIN_DIR; run: $0 build"
        exit 1
    fi
}

format_conns() {
    local c="$1"
    if [ "$c" -ge 1000 ]; then echo "$((c / 1000))k"; else echo "$c"; fi
}

format_size() {
    local s="$1"
    if [ "$s" -ge 1048576 ]; then echo "$((s / 1048576))M"
    elif [ "$s" -ge 1024 ]; then echo "$((s / 1024))K"
    else echo "${s}B"; fi
}

kill_servers() {
    pkill -f "$BIN_DIR/tcp-.*-echo" 2>/dev/null || true
    sleep 1
}

extract_json() {
    # $1: file  $2: json key — extracts numeric value (int or float)
    grep "\"$2\"" "$1" 2>/dev/null | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1
}

start_server() {
    local bin="$1" port="$2" workers="${3:-}"
    if [ -n "$workers" ]; then
        "$bin" "$port" "$workers" >/dev/null 2>&1 &
    else
        "$bin" "$port" >/dev/null 2>&1 &
    fi
    echo $!
}

snapshot_cpu() {
    # Capture per-CPU jiffies from /proc/stat -> output file (Linux only)
    [ "$CPU_SAMPLING" = true ] || { : > "$1"; return; }
    grep '^cpu[0-9]' /proc/stat > "$1"
}

calc_cpu_usage() {
    # Compare two /proc/stat snapshots, output "cpu0:XX% cpu1:YY% ..."
    local before="$1" after="$2"
    if [ ! -s "$before" ] || [ ! -s "$after" ]; then
        echo "n/a"
        return
    fi
    local result=""
    while IFS= read -r line_after; do
        local cpuname
        cpuname=$(awk '{print $1}' <<< "$line_after")
        local line_before
        line_before=$(grep "^${cpuname} " "$before")
        [ -z "$line_before" ] && continue

        # /proc/stat fields: cpu user nice system idle iowait irq softirq [steal [guest [guest_nice]]]
        # Use awk to sum all non-idle and compute idle
        local idle1 total1 idle2 total2
        idle1=$(awk '{print $5}' <<< "$line_before")
        total1=$(awk '{s=0; for(i=2;i<=NF;i++) s+=$i; print s}' <<< "$line_before")
        idle2=$(awk '{print $5}' <<< "$line_after")
        total2=$(awk '{s=0; for(i=2;i<=NF;i++) s+=$i; print s}' <<< "$line_after")

        local idle_d=$((idle2 - idle1))
        local total_d=$((total2 - total1))

        local pct=0
        if [ "$total_d" -gt 0 ]; then
            pct=$(( (total_d - idle_d) * 100 / total_d ))
        fi
        local cpunum="${cpuname#cpu}"
        result="${result:+$result }cpu${cpunum}:${pct}%"
    done < "$after"
    echo "${result:-n/a}"
}

bench_throughput() {
    local row_label="$1"          # "ST" or "MT"
    local bin_suffix="$2"         # "-echo" or "-echo-mt"
    local workers="$3"            # "" for ST, number for MT
    local conns="$4"
    local payload="$5"

    local conns_lbl size_lbl
    conns_lbl="$(format_conns "$conns")"
    size_lbl="$(format_size "$payload")"

    info "=== ${row_label} Throughput: c${conns_lbl} payload=${size_lbl} ${DURATION}s x${REPEAT} ==="

    if [ "$REPEAT" -gt 1 ]; then
        printf "  %-10s %12s %8s %10s %10s %10s  %s\n" \
            "SERVER" "msg/s(avg)" "MB/s" "p50(us)" "p99(us)" "max(us)" "runs"
    else
        printf "  %-10s %12s %8s %10s %10s %10s\n" \
            "SERVER" "msg/s" "MB/s" "p50(us)" "p99(us)" "max(us)"
    fi
    printf "  %s\n" "------------------------------------------------------------------------"

    local offset=0
    for name in "${SERVERS[@]}"; do
        local port=$((PORT_BASE + offset))
        local bin="$BIN_DIR/tcp-${name}${bin_suffix}"
        offset=$((offset + 1))

        if [ ! -x "$bin" ]; then
            warn "skip $name (binary $(basename "$bin") not found)"
            continue
        fi

        local pid
        pid="$(start_server "$bin" "$port" "$workers")"
        sleep 2

        local tp_sum=0 p50_sum=0 p99_sum=0 max_sum=0 valid_runs=0
        local tp_vals=""
        local cpu_usage_last=""

        for run in $(seq 1 "$REPEAT"); do
            local out="$RUN_DIR/throughput-${row_label,,}-c${conns_lbl}-${size_lbl}-${name}-r${run}.json"
            local cpu_before="$RUN_DIR/.cpu-before-${name}-r${run}"
            local cpu_after="$RUN_DIR/.cpu-after-${name}-r${run}"

            snapshot_cpu "$cpu_before"

            "$BIN_DIR/tcp-bench" throughput \
                -n "$conns" -d "$DURATION" -s "$payload" -p "$port" \
                > "$out" 2>/dev/null || true

            snapshot_cpu "$cpu_after"
            cpu_usage_last="$(calc_cpu_usage "$cpu_before" "$cpu_after")"
            rm -f "$cpu_before" "$cpu_after"
            if [ -s "$out" ]; then
                local tp p50 p99 lat_max
                tp=$(extract_json "$out" throughput_msg_per_sec)
                p50=$(extract_json "$out" latency_p50_us)
                p99=$(extract_json "$out" latency_p99_us)
                lat_max=$(extract_json "$out" latency_max_us)
                tp=${tp%%.*}; p50=${p50%%.*}; p99=${p99%%.*}; lat_max=${lat_max%%.*}
                if [ -n "$tp" ] && [ "$tp" -gt 0 ]; then
                    tp_sum=$((tp_sum + tp))
                    p50_sum=$((p50_sum + p50))
                    p99_sum=$((p99_sum + p99))
                    max_sum=$((max_sum + lat_max))
                    valid_runs=$((valid_runs + 1))
                    tp_vals="${tp_vals:+$tp_vals,}$tp"
                fi
            fi

            [ "$run" -lt "$REPEAT" ] && sleep 1
        done

        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        sleep 1

        if [ "$valid_runs" -gt 0 ]; then
            local tp_avg=$((tp_sum / valid_runs))
            local p50_avg=$((p50_sum / valid_runs))
            local p99_avg=$((p99_sum / valid_runs))
            local max_avg=$((max_sum / valid_runs))
            local mbps=$((tp_avg * payload / 1048576))
            if [ "$REPEAT" -gt 1 ]; then
                printf "  %-10s %12s %8s %10s %10s %10s  [%s]\n" \
                    "$name" "$tp_avg" "$mbps" "$p50_avg" "$p99_avg" "$max_avg" "$tp_vals"
            else
                printf "  %-10s %12s %8s %10s %10s %10s\n" \
                    "$name" "$tp_avg" "$mbps" "$p50_avg" "$p99_avg" "$max_avg"
            fi
            if [ "$CPU_SAMPLING" = true ]; then
                printf "  %10s cpu: %s\n" "" "$cpu_usage_last"
            fi
        else
            warn "$name: no valid output from $REPEAT runs"
        fi
    done
    echo ""
}

bench_connrate() {
    local row_label="$1"
    local bin_suffix="$2"
    local workers="$3"
    local concurrency="$4"

    local conc_lbl
    conc_lbl="$(format_conns "$concurrency")"

    info "=== ${row_label} ConnRate: concurrency=${conc_lbl} ${DURATION}s ==="

    printf "  %-10s %12s %10s\n" "SERVER" "conn/s" "fails"
    printf "  %-10s %12s %10s\n" "------" "------" "-----"

    local offset=0
    for name in "${SERVERS[@]}"; do
        local port=$((PORT_BASE + offset))
        local bin="$BIN_DIR/tcp-${name}${bin_suffix}"
        offset=$((offset + 1))

        if [ ! -x "$bin" ]; then
            warn "skip $name (binary $(basename "$bin") not found)"
            continue
        fi

        local pid
        pid="$(start_server "$bin" "$port" "$workers")"
        sleep 2

        local out="$RUN_DIR/connrate-${row_label,,}-${conc_lbl}-${name}.json"
        "$BIN_DIR/tcp-bench" connrate \
            -c "$concurrency" -d "$DURATION" -p "$port" \
            > "$out" 2>/dev/null || true

        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        sleep 1

        if [ -s "$out" ]; then
            local cps fails
            cps=$(extract_json "$out" connects_per_sec)
            fails=$(extract_json "$out" failed_connects)
            printf "  %-10s %12s %10s\n" \
                "$name" "${cps:-?}" "${fails:-0}"
        else
            warn "$name: no output"
        fi
    done
    echo ""
}

cmd_bench() {
    ensure_bin

    ulimit -n "$ULIMIT_HARD" 2>/dev/null || ulimit -n 65535 2>/dev/null || true
    kill_servers

    local nproc_val; nproc_val="$(ncpu)"
    local ts; ts="$(date +%Y%m%d-%H%M%S)"
    RUN_DIR="$RESULTS_ROOT/$ts"
    mkdir -p "$RUN_DIR"

    info "results -> $RUN_DIR   (MT workers = ${nproc_val})"
    info "servers: ${SERVERS[*]}"
    info "conns: ${CONNS[*]}  payload: ${PAYLOADS[*]}  duration: ${DURATION}s  mode: ${MODE}"
    echo ""

    # ---- Single-thread row --------------------------------------------------
    if [[ "$MODE" == "st" || "$MODE" == "both" ]]; then
        for payload in "${PAYLOADS[@]}"; do
            for conns in "${CONNS[@]}"; do
                bench_throughput ST -echo "" "$conns" "$payload"
            done
        done
        if [ "$RUN_CONNRATE" = true ]; then
            for conns in "${CONNS[@]}"; do
                bench_connrate ST -echo "" "$conns"
            done
        fi
    fi

    # ---- Multi-thread row ---------------------------------------------------
    if [[ "$MODE" == "mt" || "$MODE" == "both" ]]; then
        for payload in "${PAYLOADS[@]}"; do
            for conns in "${CONNS[@]}"; do
                bench_throughput MT -echo-mt "$nproc_val" "$conns" "$payload"
            done
        done
        if [ "$RUN_CONNRATE" = true ]; then
            for conns in "${CONNS[@]}"; do
                bench_connrate MT -echo-mt "$nproc_val" "$conns"
            done
        fi
    fi

    ok "benchmarks complete"
    info "results written to $RUN_DIR"
}

# =============================================================================
# parse bench options
# =============================================================================

# Defaults (env vars seed them; CLI options override).
# macOS narrows the default server set (libuv/boost typically absent).
if [ "$PLATFORM" = "macos" ]; then
    SERVERS=(xylem go rust)
else
    SERVERS=(xylem libuv boost go rust)
fi
IFS=',' read -ra CONNS    <<< "${CONNS:-1000,10000}"
IFS=',' read -ra PAYLOADS <<< "${PAYLOADS:-64,4096,65536}"
DURATION="${DURATION:-10}"
MODE="${MODE:-both}"
REPEAT="${REPEAT:-1}"
RUN_CONNRATE=true

parse_bench_opts() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --servers|-s)
                shift
                IFS=',' read -ra SERVERS <<< "$1"
                ;;
            --conns|-c)
                shift
                IFS=',' read -ra CONNS <<< "$1"
                ;;
            --payload|-S)
                shift
                IFS=',' read -ra PAYLOADS <<< "$1"
                ;;
            --duration|-d)
                shift
                DURATION="$1"
                ;;
            --mode|-m)
                shift
                MODE="$1"
                if [[ "$MODE" != "st" && "$MODE" != "mt" && "$MODE" != "both" ]]; then
                    err "invalid mode: $MODE (must be st|mt|both)"
                    exit 1
                fi
                ;;
            --repeat|-r)
                shift
                REPEAT="$1"
                ;;
            --no-connrate)
                RUN_CONNRATE=false
                ;;
            *)
                err "unknown bench option: $1"
                exit 1
                ;;
        esac
        shift
    done
}

# =============================================================================
# dispatcher
# =============================================================================

usage() {
    cat <<EOF
usage: $0 [install|build|bench|all] [bench-options...]

Cross-platform (Linux + macOS). Current platform: $PLATFORM

Commands:
  install   Linux: apt + rust + source-built libuv/boost (needs sudo)
            macOS: Homebrew packages
  build     xylem static lib + tcp servers (ST + MT) + tcp-bench client
  bench     run ST + MT comparison benchmarks, write benchmark/results/<ts>/
  all       install + build + bench   (default)

Bench options (pass after 'bench' or 'all'; env vars seed defaults):
  --servers, -s  xylem,go,rust     servers to compare (comma-separated)
                                   available: xylem, libuv, boost, go, rust
                                   (macOS default: xylem,go,rust)
  --conns, -c    1000,10000        connection counts (comma-separated)
  --payload, -S  64,4096,65536     payload sizes in bytes (comma-separated)
  --duration, -d 10                test duration in seconds
  --mode, -m     st|mt|both        single-thread / multi-thread / both
  --repeat, -r   3                 repeat each test N times (avg results)
  --no-connrate                    skip connection-rate tests

Notes:
  macOS uses kqueue and lacks SO_REUSEPORT / /proc; per-CPU usage sampling
  is Linux-only and its numbers are NOT comparable to the Linux suite.

Examples:
  $0 bench --servers xylem,go,rust --conns 1000 --payload 64 --duration 5
  $0 bench -s xylem,rust -c 1000,5000 -S 64,4096 -d 15 --mode st
  $0 bench --servers go,xylem --no-connrate
  REPEAT=5 DURATION=5 CONNS=1000 $0 bench   # env-var style (macOS legacy)
EOF
}

main() {
    local cmd="${1:-all}"
    shift || true

    case "$cmd" in
        install) cmd_install ;;
        build)   cmd_build   ;;
        bench)
            parse_bench_opts "$@"
            cmd_bench
            ;;
        all)
            parse_bench_opts "$@"
            cmd_install
            cmd_build
            cmd_bench
            ;;
        -h|--help|help) usage ;;
        *) err "unknown command: $cmd"; usage; exit 1 ;;
    esac
}

main "$@"
