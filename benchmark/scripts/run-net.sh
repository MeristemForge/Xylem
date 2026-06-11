#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Xylem benchmark suite (cross-platform: Linux + macOS)
# -----------------------------------------------------------------------------
#   install  - install system dependencies (Linux: sudo apt + source builds;
#              macOS: guidance via brew)
#   build    - build xylem + echo servers + bench client for each protocol
#   bench    - run comparison benchmarks and write out/results/<ts>/
#   all      - install + build + bench                             [default]
#
# Protocols (--proto, comma-separated): tcp, udp, tls
#   tcp : stream echo,   ports from 9000, ST + MT, throughput + connrate
#   udp : datagram echo, ports from 9001, ST only, throughput
#   tls : TLS-over-TCP,  ports from 9443, ST + MT, throughput + connrate
#         (xylem built with -DXYLEM_ENABLE_TLS=ON; servers link OpenSSL)
#
# Compared servers: xylem, go, rust for all protocols. Override with
# --servers. Missing binaries are skipped automatically. UDP has no MT row
# (the public UDP API exposes no SO_REUSEPORT, so a single bound port cannot
# be fanned across workers).
#
# NOTE: macOS uses kqueue and lacks SO_REUSEPORT / /proc; its numbers are
# NOT comparable to the Linux suite. Per-CPU usage sampling is Linux-only.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
NET_DIR="$BENCH_DIR/net"          # protocol suites: net/tcp, net/udp, net/tls
OUT_DIR="$BENCH_DIR/out"          # all build output lives here
BIN_DIR="$OUT_DIR"                # compiled binaries go straight into out/
BUILD_DIR="$OUT_DIR/build"        # xylem CMake build tree
RESULTS_ROOT="$OUT_DIR/results"

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
# per-protocol configuration
# =============================================================================
# Sets PROTO_PORT_BASE / PROTO_HAS_MT / PROTO_HAS_CONNRATE / PROTO_TLS for the
# current protocol $1.
proto_config() {
    case "$1" in
        tcp) PROTO_PORT_BASE=9000; PROTO_HAS_MT=true;  PROTO_HAS_CONNRATE=true;  PROTO_TLS=false ;;
        udp) PROTO_PORT_BASE=9001; PROTO_HAS_MT=false; PROTO_HAS_CONNRATE=false; PROTO_TLS=false ;;
        tls) PROTO_PORT_BASE=9443; PROTO_HAS_MT=true;  PROTO_HAS_CONNRATE=true;  PROTO_TLS=true  ;;
        *)   err "unknown protocol: $1 (must be tcp|udp|tls)"; exit 1 ;;
    esac
}

# Does the protocol list contain tls? (decides the xylem TLS build flag)
protos_need_tls() {
    local p
    for p in "${PROTOS[@]}"; do
        [ "$p" = "tls" ] && return 0
    done
    return 1
}

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
        local BREW_PKGS=(cmake ninja pkg-config openssl go rust)
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

    ok "all dependencies ready"
}

# =============================================================================
# build
# =============================================================================

CFLAGS_COMMON="-O3 -DNDEBUG -flto -Wall -Wextra"
LDFLAGS_COMMON="-s -flto"

# OpenSSL location for the TLS suite. Set OPENSSL_ROOT to a custom OpenSSL
# install/build tree (e.g. one built from source when the distro's libssl is
# older than the version CMakeLists requires); empty uses the system OpenSSL.
OSSL_INC=""
OSSL_LIB="-lssl -lcrypto"
if [ -n "${OPENSSL_ROOT:-}" ]; then
    OSSL_INC="-I${OPENSSL_ROOT}/include"
    OSSL_LIB="-L${OPENSSL_ROOT} -Wl,-rpath,${OPENSSL_ROOT} -lssl -lcrypto"
fi

# Build all servers + client for a single protocol $1 against $XYLEM_LIB.
build_proto() {
    local proto="$1"
    proto_config "$proto"

    local xylem_extra="-lpthread"
    local client_extra=""
    local inc_extra=""
    if [ "$PROTO_TLS" = true ]; then
        xylem_extra="-lpthread $OSSL_LIB"
        client_extra="$OSSL_LIB"
        inc_extra="$OSSL_INC"
    fi

    # which suffixes to build: "" (ST) always, "-mt" only if the proto has MT
    local suffixes=("")
    [ "$PROTO_HAS_MT" = true ] && suffixes+=("-mt")

    local suf src
    for suf in "${suffixes[@]}"; do
        local label="ST"; [ -n "$suf" ] && label="MT"
        info "building ${proto} servers (${label})..."

        # xylem
        src="$NET_DIR/${proto}/server/xylem-echo/server${suf}.c"
        if [ -f "$src" ]; then
            # shellcheck disable=SC2086
            gcc $CFLAGS_COMMON $inc_extra -I"$PROJECT_ROOT/include" "$src" \
                "$XYLEM_LIB" $xylem_extra $LDFLAGS_COMMON \
                -o "$BIN_DIR/${proto}-xylem-echo${suf}" \
                || warn "skip xylem ${label} (build failed)"
        fi

        # go (single module with per-mode command subpackages: echo, echo-mt)
        local godir="$NET_DIR/${proto}/server/go-echo"
        if [ -d "$godir/echo${suf}" ] && command -v go >/dev/null 2>&1; then
            ( cd "$godir" && \
              CGO_ENABLED=0 go build -ldflags="-s -w" \
                  -o "$BIN_DIR/${proto}-go-echo${suf}" "./echo${suf}" ) \
              || warn "skip go ${label} (build failed)"
        fi

        # rust (single crate with per-bin targets)
        if [ -d "$NET_DIR/${proto}/server/rust-echo" ] && command -v cargo >/dev/null 2>&1; then
            ( cd "$NET_DIR/${proto}/server/rust-echo" && \
              cargo build --release -q --bin "${proto}-rust-echo${suf}" && \
              cp "target/release/${proto}-rust-echo${suf}" "$BIN_DIR/" && \
              strip "$BIN_DIR/${proto}-rust-echo${suf}" ) \
              || warn "skip rust ${label} (build failed)"
        fi
    done

    info "building ${proto}-bench client..."
    # shellcheck disable=SC2086
    gcc $CFLAGS_COMMON $inc_extra "$NET_DIR/${proto}/client/client.c" \
        $client_extra $LDFLAGS_COMMON -o "$BIN_DIR/${proto}-bench" \
        || { err "failed to build ${proto}-bench client"; return 1; }
    ok "${proto}-bench built"
}

cmd_build() {
    mkdir -p "$BIN_DIR"

    local tls_flag="OFF"
    if protos_need_tls; then tls_flag="ON"; fi

    local ossl_cmake=""
    if [ -n "${OPENSSL_ROOT:-}" ]; then
        ossl_cmake="-DOPENSSL_ROOT_DIR=$OPENSSL_ROOT"
    fi

    info "building xylem static library (XYLEM_ENABLE_TLS=${tls_flag})..."
    # shellcheck disable=SC2086
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS='-O3 -DNDEBUG -flto' \
        -DXYLEM_ENABLE_TLS="$tls_flag" \
        $ossl_cmake \
        -G Ninja
    ninja -C "$BUILD_DIR" xylem -j"$(ncpu)"
    XYLEM_LIB="$BUILD_DIR/libxylem.a"
    ok "xylem built"

    local proto
    for proto in "${PROTOS[@]}"; do
        build_proto "$proto"
    done

    echo ""
    ls -lh "$BIN_DIR"
}

# =============================================================================
# bench
# =============================================================================

PORT_BASE="${PORT_BASE:-9000}"   # overridden per-proto during bench

ensure_bin() {
    local proto
    for proto in "${PROTOS[@]}"; do
        if [ ! -x "$BIN_DIR/${proto}-bench" ]; then
            err "binaries missing in $BIN_DIR; run: $0 build --proto $(IFS=,; echo "${PROTOS[*]}")"
            exit 1
        fi
    done
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
    pkill -f "$BIN_DIR/${CUR_PROTO}-.*-echo" 2>/dev/null || true
    sleep 1
}

extract_json() {
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
    [ "$CPU_SAMPLING" = true ] || { : > "$1"; return; }
    grep '^cpu[0-9]' /proc/stat > "$1"
}

calc_cpu_usage() {
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

# Peak resident set size (KB) of a server PID, including child threads.
proc_peak_rss_kb() {
    local pid="$1"
    if [ "$PLATFORM" = "linux" ]; then
        [ -r "/proc/$pid/status" ] || { echo ""; return; }
        awk '/^VmHWM:/ {print $2}' "/proc/$pid/status" 2>/dev/null
    else
        ps -o rss= -p "$pid" 2>/dev/null | tr -d ' '
    fi
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

    info "=== [${CUR_PROTO}] ${row_label} Throughput: c${conns_lbl} payload=${size_lbl} ${DURATION}s x${REPEAT} ==="

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
        local bin="$BIN_DIR/${CUR_PROTO}-${name}${bin_suffix}"
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
            local row_lc; row_lc="$(printf '%s' "$row_label" | tr 'A-Z' 'a-z')"
            local out="$RUN_DIR/${CUR_PROTO}-throughput-${row_lc}-c${conns_lbl}-${size_lbl}-${name}-r${run}.json"
            local cpu_before="$RUN_DIR/.cpu-before-${name}-r${run}"
            local cpu_after="$RUN_DIR/.cpu-after-${name}-r${run}"

            snapshot_cpu "$cpu_before"

            "$BIN_DIR/${CUR_PROTO}-bench" throughput \
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

        local srv_peak_rss; srv_peak_rss="$(proc_peak_rss_kb "$pid")"

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
            printf "  %10s srv: peak_rss=%s\n" "" \
                "$([ -n "$srv_peak_rss" ] && echo "$((srv_peak_rss / 1024))MB" || echo n/a)"
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

    info "=== [${CUR_PROTO}] ${row_label} ConnRate: concurrency=${conc_lbl} ${DURATION}s ==="

    printf "  %-10s %12s %10s\n" "SERVER" "conn/s" "fails"
    printf "  %-10s %12s %10s\n" "------" "------" "-----"

    local offset=0
    for name in "${SERVERS[@]}"; do
        local port=$((PORT_BASE + offset))
        local bin="$BIN_DIR/${CUR_PROTO}-${name}${bin_suffix}"
        offset=$((offset + 1))

        if [ ! -x "$bin" ]; then
            warn "skip $name (binary $(basename "$bin") not found)"
            continue
        fi

        local pid
        pid="$(start_server "$bin" "$port" "$workers")"
        sleep 2

        local row_lc; row_lc="$(printf '%s' "$row_label" | tr 'A-Z' 'a-z')"
        local out="$RUN_DIR/${CUR_PROTO}-connrate-${row_lc}-${conc_lbl}-${name}.json"
        "$BIN_DIR/${CUR_PROTO}-bench" connrate \
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

# Run the full matrix for one protocol.
bench_proto() {
    CUR_PROTO="$1"
    proto_config "$CUR_PROTO"
    PORT_BASE="$PROTO_PORT_BASE"

    local nproc_val; nproc_val="$(ncpu)"

    kill_servers

    info "=== protocol: ${CUR_PROTO}  (port base ${PORT_BASE}) ==="
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
        if [ "$RUN_CONNRATE" = true ] && [ "$PROTO_HAS_CONNRATE" = true ]; then
            for conns in "${CONNS[@]}"; do
                bench_connrate ST -echo "" "$conns"
            done
        fi
    fi

    # ---- Multi-thread row (skipped for protocols without MT, e.g. udp) ------
    if [[ "$MODE" == "mt" || "$MODE" == "both" ]] && [ "$PROTO_HAS_MT" = true ]; then
        for payload in "${PAYLOADS[@]}"; do
            for conns in "${CONNS[@]}"; do
                bench_throughput MT -echo-mt "$nproc_val" "$conns" "$payload"
            done
        done
        if [ "$RUN_CONNRATE" = true ] && [ "$PROTO_HAS_CONNRATE" = true ]; then
            for conns in "${CONNS[@]}"; do
                bench_connrate MT -echo-mt "$nproc_val" "$conns"
            done
        fi
    elif [[ "$MODE" == "mt" || "$MODE" == "both" ]] && [ "$PROTO_HAS_MT" = false ]; then
        info "[${CUR_PROTO}] no MT row for this protocol; skipping."
        echo ""
    fi
}

cmd_bench() {
    ensure_bin

    ulimit -n "$ULIMIT_HARD" 2>/dev/null || ulimit -n 65535 2>/dev/null || true

    local ts; ts="$(date +%Y%m%d-%H%M%S)"
    RUN_DIR="$RESULTS_ROOT/$ts"
    mkdir -p "$RUN_DIR"

    local nproc_val; nproc_val="$(ncpu)"
    info "results -> $RUN_DIR   (MT workers = ${nproc_val})"
    info "protocols: ${PROTOS[*]}"
    echo ""

    local proto
    for proto in "${PROTOS[@]}"; do
        bench_proto "$proto"
    done

    ok "benchmarks complete"
    info "results written to $RUN_DIR"
}

# =============================================================================
# parse bench options
# =============================================================================

SERVERS=(xylem go rust)
IFS=',' read -ra PROTOS    <<< "${PROTO:-tcp}"
IFS=',' read -ra CONNS     <<< "${CONNS:-1000,10000}"
IFS=',' read -ra PAYLOADS  <<< "${PAYLOADS:-64,4096,65536}"
DURATION="${DURATION:-10}"
MODE="${MODE:-both}"
REPEAT="${REPEAT:-1}"
RUN_CONNRATE=true

parse_bench_opts() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --proto|-P)
                shift
                IFS=',' read -ra PROTOS <<< "$1"
                ;;
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

    # validate protocols up front
    local p
    for p in "${PROTOS[@]}"; do proto_config "$p"; done
}

# =============================================================================
# dispatcher
# =============================================================================

usage() {
    cat <<EOF
usage: $0 [install|build|bench|all] [options...]

Cross-platform (Linux + macOS). Current platform: $PLATFORM

Commands:
  install   Linux: apt + rust (needs sudo)
            macOS: Homebrew packages
  build     xylem static lib + per-protocol servers + bench clients
  bench     run comparison benchmarks, write benchmark/out/results/<ts>/
  all       install + build + bench   (default)

Options (pass after the command; env vars seed defaults):
  --proto, -P    tcp,udp,tls        protocols to build/bench (default: tcp)
                                    tcp/tls: ST+MT, throughput+connrate
                                    udp: ST only, throughput
  --servers, -s  xylem,go,rust      servers to compare (comma-separated)
                                    available: xylem, go, rust
  --conns, -c    1000,10000         connection counts (comma-separated)
  --payload, -S  64,4096,65536      payload sizes in bytes (comma-separated)
  --duration, -d 10                 test duration in seconds
  --mode, -m     st|mt|both         single-thread / multi-thread / both
  --repeat, -r   3                  repeat each test N times (avg results)
  --no-connrate                     skip connection-rate tests

Notes:
  TLS requires OpenSSL; xylem is built with -DXYLEM_ENABLE_TLS=ON when tls is
  among the protocols. UDP has no MT row.
  macOS uses kqueue and lacks SO_REUSEPORT / /proc; numbers are NOT comparable
  to the Linux suite.

Examples:
  $0 build --proto tcp,udp,tls
  $0 bench --proto tls --servers xylem,go,rust --conns 1000 --duration 5
  $0 bench -P udp -s xylem,rust -c 1000,5000 -d 15 --mode st
  $0 all --proto tcp,udp,tls --duration 15
EOF
}

main() {
    local cmd="${1:-all}"
    shift || true

    case "$cmd" in
        install) cmd_install ;;
        build)
            parse_bench_opts "$@"
            cmd_build
            ;;
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
