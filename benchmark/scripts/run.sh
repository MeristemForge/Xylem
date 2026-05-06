#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Xylem TCP benchmark suite
# -----------------------------------------------------------------------------
#   install  - install system dependencies (requires sudo)
#   build    - build xylem + all TCP echo servers (ST + MT) + bench client
#   bench    - run comparison benchmarks and write results/<ts>/
#   all      - install + build + bench                             [default]
#
# Compared servers (5 families):
#   xylem, libuv, boost, go, rust
# Each family has a single-threaded (ST) and multi-threaded (MT) binary.
#
# Fairness rules for MT servers:
#   - MT workers run as N pthreads / N goroutines / N tokio workers.
#   - Each worker has its own listen socket with SO_REUSEPORT so the
#     kernel load-balances accepts (except xylem/go/rust which use
#     their native shared-runtime work-stealing -- each project's
#     idiomatic MT story).
#   - TCP_NODELAY on accepted sockets, backlog 4096, 64 KB read buffer.
#
# Benchmark matrix
#   ST row : payload in {64B, 4KB, 64KB} x conns in {1k, 10k}  = 6 runs / family
#   MT row : same matrix with workers = $(nproc)                = 6 runs / family
#   ConnRate : concurrency in {1k, 10k}                         = 2 runs x 2 rows
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
# install
# =============================================================================

cmd_install() {
    if [ "$(id -u)" -ne 0 ]; then
        info "escalating to root for dependency install..."
        exec sudo -E "$0" install
    fi

    local LIBUV_VERSION="1.49.2"
    local BOOST_VERSION="1.87.0"
    local PREFIX="/usr/local"
    local JOBS; JOBS="$(nproc)"

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
    ninja -C "$BUILD_DIR" xylem -j"$(nproc)"
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
        # Library linker flags: libuv -> -luv.
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
    # xylem MT
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

SERVERS=(xylem libuv boost go rust)
DURATION=10
PORT_BASE=9000

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
    if [ "$s" -ge 1024 ]; then echo "$((s / 1024))K"; else echo "${s}B"; fi
}

kill_servers() {
    pkill -f "$BIN_DIR/tcp-.*-echo" 2>/dev/null || true
    sleep 1
}

extract_num() {
    # $1: file  $2: json key
    grep "\"$2\"" "$1" 2>/dev/null | grep -oE '[0-9]+' | tail -1
}

# start_server <binary> <port> [workers]
start_server() {
    local bin="$1" port="$2" workers="${3:-}"
    if [ -n "$workers" ]; then
        "$bin" "$port" "$workers" >/dev/null 2>&1 &
    else
        "$bin" "$port" >/dev/null 2>&1 &
    fi
    echo $!
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

    info "=== ${row_label} Throughput: c${conns_lbl} payload=${size_lbl} ${DURATION}s ==="

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

        local out="$RUN_DIR/throughput-${row_label,,}-c${conns_lbl}-${size_lbl}-${name}.json"
        "$BIN_DIR/tcp-bench" throughput \
            -n "$conns" -d "$DURATION" -s "$payload" -p "$port" \
            > "$out" 2>/dev/null || true

        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        sleep 1

        if [ -s "$out" ]; then
            local tp p50 p99
            tp=$(extract_num "$out" throughput_msg_per_sec)
            p50=$(extract_num "$out" latency_p50_us)
            p99=$(extract_num "$out" latency_p99_us)
            local mbps=0
            [ -n "$tp" ] && mbps=$(( tp * payload / 1048576 ))
            printf "  %-10s %10s msg/s  %6s MB/s  p50=%5s us  p99=%6s us\n" \
                "$name" "${tp:-?}" "$mbps" "${p50:-?}" "${p99:-?}"
        else
            warn "$name: no output"
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
            cps=$(extract_num "$out" connects_per_sec)
            fails=$(extract_num "$out" failed_connects)
            printf "  %-10s %10s conn/s  fails=%s\n" \
                "$name" "${cps:-?}" "${fails:-0}"
        else
            warn "$name: no output"
        fi
    done
    echo ""
}

cmd_bench() {
    ensure_bin

    ulimit -n 200000 2>/dev/null || ulimit -n 65535 2>/dev/null || true
    kill_servers

    local nproc_val; nproc_val="$(nproc)"
    local ts; ts="$(date +%Y%m%d-%H%M%S)"
    RUN_DIR="$RESULTS_ROOT/$ts"
    mkdir -p "$RUN_DIR"

    info "results -> $RUN_DIR   (MT workers = ${nproc_val})"
    echo ""

    # ---- Single-thread row --------------------------------------------------
    for payload in 64 4096 65536; do
        for conns in 1000 10000; do
            bench_throughput ST -echo "" "$conns" "$payload"
        done
    done
    for concurrency in 1000 10000; do
        bench_connrate ST -echo "" "$concurrency"
    done

    # ---- Multi-thread row ---------------------------------------------------
    for payload in 64 4096 65536; do
        for conns in 1000 10000; do
            bench_throughput MT -echo-mt "$nproc_val" "$conns" "$payload"
        done
    done
    for concurrency in 1000 10000; do
        bench_connrate MT -echo-mt "$nproc_val" "$concurrency"
    done

    ok "benchmarks complete"
    info "results written to $RUN_DIR"
}

# =============================================================================
# dispatcher
# =============================================================================

usage() {
    cat <<EOF
usage: $0 [install|build|bench|all]

  install   apt packages, rust, libuv/boost (needs sudo)
  build     xylem static lib + tcp servers (ST + MT) + tcp-bench client
  bench     run ST + MT comparison benchmarks, write benchmark/results/<ts>/
  all       install + build + bench   (default)
EOF
}

main() {
    local cmd="${1:-all}"
    case "$cmd" in
        install) cmd_install ;;
        build)   cmd_build   ;;
        bench)   cmd_bench   ;;
        all)     cmd_install; cmd_build; cmd_bench ;;
        -h|--help|help) usage ;;
        *) err "unknown command: $cmd"; usage; exit 1 ;;
    esac
}

main "$@"
