#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Xylem benchmark suite (cross-platform: Linux + macOS)
# -----------------------------------------------------------------------------
# Usage:  ./run-net.sh <proto>     - build + run the full comparison matrix
#                                     for one protocol, e.g.:
#        ./run-net.sh tcp     TCP:   stream echo,  ports from 9000,
#                                    ST + MT, throughput + connrate
#        ./run-net.sh udp     UDP:   datagram echo, ports from 9001,
#                                    ST only, throughput
#        ./run-net.sh tls     TLS:   TLS-over-TCP, ports from 9443,
#                                    ST + MT, throughput + connrate
#                                    (xylem built with -DXYLEM_ENABLE_TLS=ON;
#                                    servers link OpenSSL)
#        ./run-net.sh              - same, all protocols (tcp,udp,tls)
#        ./run-net.sh help         - usage
#
# Missing dependencies are installed automatically when the run starts
# (Linux: sudo apt + rust, macOS: brew; openssl/libssl-dev only when tls is
# among the requested protocols).
#
# Compared servers: xylem, go, rust. Missing binaries are skipped automatically.
# UDP has no MT row (the public UDP API exposes no SO_REUSEPORT, so a single
# bound port cannot be fanned across workers).
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
# core pinning (single-host fairness)
# =============================================================================
# On one box the client and server compete for cores; a busy load generator
# then steals CPU from the server and distorts its numbers. Pin them to
# DISJOINT core sets sized from the actual core count: the high-numbered cores
# drive load, the low-numbered cores run the server.
#
#   PIN=auto|on|off  (default auto: on when >=4 cores and taskset exists)
#   CLIENT_NCPU=<n>  override the load-generator core count
#
# Layout for N cores with C client cores (S = N - C server cores):
#   client : cores [S .. N-1]
#   server : cores [0 .. S-1]   (MT uses S workers)   |   core 0 only (ST)
PIN_REQUEST="${PIN:-auto}"
PIN_ENABLE=false
NCPU_TOTAL="$(ncpu)"
if [ "$PIN_REQUEST" != off ] \
   && command -v taskset >/dev/null 2>&1 \
   && [ "$NCPU_TOTAL" -ge 4 ]; then
    PIN_ENABLE=true
fi
[ "$PIN_REQUEST" = on ] && PIN_ENABLE=true

if [ "$PIN_ENABLE" = true ]; then
    # Ping-pong echo is symmetric: the client does as much work as the server
    # (same read/write/syscalls, and for TLS the same crypto). Split the cores
    # 50/50 so the load generator can never cap the server. Server measured on
    # N/2 cores; compare servers against each other on that same set.
    CLIENT_NCPU="${CLIENT_NCPU:-$(( NCPU_TOTAL / 2 ))}"
    [ "$CLIENT_NCPU" -lt 1 ] && CLIENT_NCPU=1
    [ "$CLIENT_NCPU" -gt $(( NCPU_TOTAL - 1 )) ] && CLIENT_NCPU=$(( NCPU_TOTAL - 1 ))
    SERVER_NCPU=$(( NCPU_TOTAL - CLIENT_NCPU ))
    SERVER_CORES="0-$(( SERVER_NCPU - 1 ))"
    SERVER_CORE_ST="0"
    CLIENT_CORES="${SERVER_NCPU}-$(( NCPU_TOTAL - 1 ))"
else
    SERVER_NCPU="$NCPU_TOTAL"   # MT worker count when not pinning
fi

# Wrap the bench client so it runs on the client core set (and, for the Go
# tcp client, with a matching GOMAXPROCS). A no-op when pinning is disabled.
run_client() {
    if [ "$PIN_ENABLE" = true ]; then
        GOMAXPROCS="$CLIENT_NCPU" taskset -c "$CLIENT_CORES" "$@"
    else
        "$@"
    fi
}

# =============================================================================
# per-protocol configuration
# =============================================================================
# Sets PROTO_PORT_BASE / PROTO_HAS_MT / PROTO_HAS_CONNRATE / PROTO_TLS for the
# current protocol $1.
proto_config() {
    case "$1" in
        tcp) PROTO_PORT_BASE=9000; PROTO_HAS_MT=true;  PROTO_HAS_CONNRATE=true;  PROTO_TLS=false; PROTO_CONNS="1000,10000"; PROTO_PAYLOADS="64,4096,65536" ;;
        udp) PROTO_PORT_BASE=9001; PROTO_HAS_MT=false; PROTO_HAS_CONNRATE=false; PROTO_TLS=false; PROTO_CONNS="1";           PROTO_PAYLOADS="64,1400" ;;
        tls) PROTO_PORT_BASE=9443; PROTO_HAS_MT=true;  PROTO_HAS_CONNRATE=true;  PROTO_TLS=true;  PROTO_CONNS="1000,10000"; PROTO_PAYLOADS="64,4096,65536" ;;
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
# dependencies (auto-installed at run time when missing)
# =============================================================================

# Fill MISSING_DEPS with anything not installed. Tools are probed with
# command -v so installs via brew, official installers, or rustup are all
# recognized; libraries (libssl-dev, brew openssl) are checked only when tls
# is among the requested protocols (and no OPENSSL_ROOT override is set).
find_missing_deps() {
    MISSING_DEPS=()
    local tool
    for tool in cmake ninja pkg-config go; do
        command -v "$tool" >/dev/null 2>&1 || MISSING_DEPS+=("$tool")
    done
    command -v cargo >/dev/null 2>&1 || MISSING_DEPS+=("rust")
    if [ "$PLATFORM" = "macos" ]; then
        if protos_need_tls && [ -z "${OPENSSL_ROOT:-}" ] \
           && ! brew list openssl >/dev/null 2>&1; then
            MISSING_DEPS+=("openssl")
        fi
    else
        for tool in autoconf automake libtool curl git; do
            command -v "$tool" >/dev/null 2>&1 || MISSING_DEPS+=("$tool")
        done
        dpkg -s build-essential >/dev/null 2>&1 || MISSING_DEPS+=("build-essential")
        if protos_need_tls; then
            dpkg -s libssl-dev >/dev/null 2>&1 || MISSING_DEPS+=("libssl-dev")
        fi
    fi
    [ "${#MISSING_DEPS[@]}" -eq 0 ]
}

# Install MISSING_DEPS. Names are brew formulas on macOS and apt packages on
# Linux (with a few renames: ninja->ninja-build, go->golang-go, rust->rustup);
# apt needs root, rust installs as the invoking user.
install_missing() {
    info "installing missing dependencies: ${MISSING_DEPS[*]}"
    if [ "$PLATFORM" = "macos" ]; then
        brew install "${MISSING_DEPS[@]}"
        ok "all dependencies ready (macOS)"
        return
    fi

    local apt_pkgs=() rust_needed=false d
    for d in "${MISSING_DEPS[@]}"; do
        case "$d" in
            rust)  rust_needed=true ;;
            ninja) apt_pkgs+=("ninja-build") ;;
            go)    apt_pkgs+=("golang-go") ;;
            *)     apt_pkgs+=("$d") ;;
        esac
    done
    if [ "${#apt_pkgs[@]}" -gt 0 ]; then
        if [ "$(id -u)" -ne 0 ]; then
            sudo -E apt-get update -qq
            sudo -E apt-get install -y -qq "${apt_pkgs[@]}"
        else
            apt-get update -qq
            apt-get install -y -qq "${apt_pkgs[@]}"
        fi
        ok "apt packages ready"
    fi
    if [ "$rust_needed" = true ]; then
        # shellcheck disable=SC2016
        curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --quiet
        ok "rust ready"
    fi
    ok "all dependencies ready"
}

# Called at the start of every run; installs anything that is missing.
ensure_deps() {
    find_missing_deps || install_missing
}

# =============================================================================
# build
# =============================================================================

CFLAGS_COMMON="-O3 -DNDEBUG -flto -Wall -Wextra"
LDFLAGS_COMMON="-s -flto"

# OpenSSL location for the TLS suite. Set OPENSSL_ROOT to a custom OpenSSL
# install/build tree (e.g. one built from source when the distro's libssl is
# older than the version CMakeLists requires); empty uses the system OpenSSL.
#
# The shared objects live in different places depending on how OpenSSL was
# produced: an installed prefix puts them under lib64/ or lib/, while an
# in-place source build leaves them at the tree root. Probe all three and add
# -L/-rpath for whichever actually holds libssl, so linking and run time both
# resolve the custom build instead of falling back to the system copy.
OSSL_INC=""
OSSL_LIB="-lssl -lcrypto"
if [ -n "${OPENSSL_ROOT:-}" ]; then
    OSSL_INC="-I${OPENSSL_ROOT}/include"
    OSSL_LIB=""
    for d in "${OPENSSL_ROOT}/lib64" "${OPENSSL_ROOT}/lib" "${OPENSSL_ROOT}"; do
        for f in "$d"/libssl.so* "$d"/libssl.dylib "$d"/libssl.a; do
            # Unmatched globs stay literal, so test each candidate with -e
            # rather than letting one missing form fail the whole check.
            if [ -e "$f" ]; then
                OSSL_LIB="${OSSL_LIB} -L${d} -Wl,-rpath,${d}"
                break
            fi
        done
    done
    if [ -z "$OSSL_LIB" ]; then
        warn "no libssl found under ${OPENSSL_ROOT}{/lib64,/lib,}; using -L${OPENSSL_ROOT}"
        OSSL_LIB="-L${OPENSSL_ROOT} -Wl,-rpath,${OPENSSL_ROOT}"
    fi
    OSSL_LIB="${OSSL_LIB} -lssl -lcrypto"
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

        # rust (single crate with per-bin targets; shared build cache in out/)
        if [ -d "$NET_DIR/${proto}/server/rust-echo" ] && command -v cargo >/dev/null 2>&1; then
            ( cd "$NET_DIR/${proto}/server/rust-echo" && \
              cargo build --release -q --target-dir "$BIN_DIR/cargo" \
                  --bin "${proto}-rust-echo${suf}" && \
              cp "$BIN_DIR/cargo/release/${proto}-rust-echo${suf}" "$BIN_DIR/" && \
              strip "$BIN_DIR/${proto}-rust-echo${suf}" ) \
              || warn "skip rust ${label} (build failed)"
        fi

    done

    info "building ${proto}-bench client..."
    # All bench clients are Go programs (multi-core load generators). The old
    # C epoll/IOCP clients were single-threaded and capped client-side load;
    # the tls client also dropped its OpenSSL dependency (uses crypto/tls).
    ( cd "$NET_DIR/${proto}/client" && \
      CGO_ENABLED=0 go build -ldflags="-s -w" -o "$BIN_DIR/${proto}-bench" . ) \
      || { err "failed to build ${proto}-bench (go) client"; return 1; }
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
    # out/build is shared between the Linux and Windows drivers; a cache
    # generated by the other platform records its own absolute paths and CMake
    # refuses to reuse it. Always start from a fresh build tree.
    rm -rf "$BUILD_DIR"
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

ensure_bin() {
    local proto
    for proto in "${PROTOS[@]}"; do
        if [ ! -x "$BIN_DIR/${proto}-bench" ]; then
            err "binaries missing in $BIN_DIR; run: $0 build"
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

# Start a server in the background and leave its PID in SRV_PID. Run it from
# BIN_DIR: servers that generate files (the TLS certs) write them relative to
# their working directory, so they land in out/ instead of wherever the driver
# was invoked from. Must not be called inside $() -- backgrounding within a
# command substitution reaps the job when the substitution shell exits.
start_server() {
    local bin="$1" port="$2" workers="${3:-}"
    local pin=""
    if [ "$PIN_ENABLE" = true ]; then
        # MT (workers set) gets the server core block; ST gets a single core.
        local cores="$SERVER_CORES"
        [ -z "$workers" ] && cores="$SERVER_CORE_ST"
        pin="taskset -c $cores"
    fi
    if [ -n "$workers" ]; then
        ( cd "$BIN_DIR" && $pin "$bin" "$port" "$workers" >/dev/null 2>&1 ) &
    else
        ( cd "$BIN_DIR" && $pin "$bin" "$port" >/dev/null 2>&1 ) &
    fi
    SRV_PID=$!
}

snapshot_cpu() {
    [ "$CPU_SAMPLING" = true ] || { : > "$1"; return; }
    grep '^cpu[0-9]' /proc/stat > "$1" || true
}

# Average busy% and per-core busy% for the cores in a spec ("0-7" or single
# "0") between two /proc/stat snapshots, in one awk pass. With pinning on the
# spec is the server's core set, so this reads as "how saturated is the server
# on its own cores". Prints two lines: the per-core list ("cpu0:95% cpu1:.."),
# then the average ("NN%"); both are "n/a" when either snapshot is unusable.
calc_cpu_stats() {
    local before="$1" after="$2" spec="$3"
    if [ ! -s "$before" ] || [ ! -s "$after" ]; then
        echo "n/a"
        echo "n/a"
        return
    fi
    local lo hi
    if [[ "$spec" == *-* ]]; then
        lo="${spec%-*}"; hi="${spec#*-}"
    else
        lo="$spec"; hi="$spec"
    fi
    awk -v lo="$lo" -v hi="$hi" '
        NR == FNR {                       # first file: per-cpu totals + idle
            t1[$1] = 0
            for (i = 2; i <= NF; i++) t1[$1] += $i
            i1[$1] = $5
            next
        }
        {                                 # second file: same fields
            t2[$1] = 0
            for (i = 2; i <= NF; i++) t2[$1] += $i
            i2[$1] = $5
        }
        END {
            line = ""
            sbusy = 0
            stotal = 0
            for (c = lo; c <= hi; c++) {
                name = "cpu" c
                if (!(name in t1) || !(name in t2)) continue
                td = t2[name] - t1[name]
                busy = td - (i2[name] - i1[name])
                pct = 0
                if (td > 0) pct = int(busy * 100 / td)
                line = (line == "") ? name ":" pct "%" : line " " name ":" pct "%"
                sbusy += busy
                stotal += td
            }
            if (line == "") line = "n/a"
            avg = "n/a"
            if (stotal > 0) avg = int(sbusy * 100 / stotal) "%"
            print line
            print avg
        }' "$before" "$after"
}

# Core spec the server is (or would be) running on, for the given row.
server_core_spec() {
    local row_label="$1"
    if [ "$PIN_ENABLE" = true ]; then
        [ "$row_label" = "ST" ] && { echo "$SERVER_CORE_ST"; return; }
        echo "$SERVER_CORES"
    else
        echo "0-$(( NCPU_TOTAL - 1 ))"
    fi
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

    # Loop-invariant for this row: the lowercase run name and the core spec
    # the server runs on (both depend only on the row label).
    local row_lc spec
    row_lc="$(printf '%s' "$row_label" | tr 'A-Z' 'a-z')"
    spec="$(server_core_spec "$row_label")"

    local warmup_label=""
    if [ "$BENCH_WARMUP_RUNS" -gt 0 ]; then
        warmup_label=" (+${BENCH_WARMUP_RUNS} warmup)"
    fi
    info "=== [${CUR_PROTO}] ${row_label} Throughput: c${conns_lbl} payload=${size_lbl} ${DURATION}s${warmup_label} ==="

    printf "  %-10s %12s %8s %10s %10s %10s\n" \
        "SERVER" "msg/s" "MB/s" "p50(us)" "p99(us)" "max(us)"
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

        start_server "$bin" "$port" "$workers"
        local pid="$SRV_PID"
        sleep 2

        local tp_sum=0 p50_sum=0 p99_sum=0 max_sum=0 valid_runs=0
        local cpu_usage_last=""
        local srv_cpu_last=""

        local total_runs=$((BENCH_WARMUP_RUNS + 1))
        for run in $(seq 1 "$total_runs"); do
            local measured_run=$((run - BENCH_WARMUP_RUNS))
            local run_name="r${measured_run}"
            if [ "$measured_run" -le 0 ]; then
                run_name="warmup$run"
            fi
            local out="$RUN_DIR/${CUR_PROTO}-throughput-${row_lc}-c${conns_lbl}-${size_lbl}-${name}-${run_name}.json"
            local cpu_before="$RUN_DIR/.cpu-before-${name}-${run_name}"
            local cpu_after="$RUN_DIR/.cpu-after-${name}-${run_name}"
            local cpu_client_before="$RUN_DIR/.cpu-window-before-${name}-${run_name}"
            local cpu_client_after="$RUN_DIR/.cpu-window-after-${name}-${run_name}"
            rm -f "$cpu_client_before" "$cpu_client_after"

            snapshot_cpu "$cpu_before"

            export BENCH_CPU_BEFORE_FILE="$cpu_client_before"
            export BENCH_CPU_AFTER_FILE="$cpu_client_after"
            run_client "$BIN_DIR/${CUR_PROTO}-bench" throughput \
                -n "$conns" -d "$DURATION" -s "$payload" -p "$port" \
                > "$out" 2>/dev/null || true
            unset BENCH_CPU_BEFORE_FILE BENCH_CPU_AFTER_FILE

            snapshot_cpu "$cpu_after"
            local cpu_calc_before="$cpu_before"
            local cpu_calc_after="$cpu_after"
            if [ -s "$cpu_client_before" ] && [ -s "$cpu_client_after" ]; then
                cpu_calc_before="$cpu_client_before"
                cpu_calc_after="$cpu_client_after"
            fi
            read -r cpu_usage_last srv_cpu_last < <(calc_cpu_stats "$cpu_calc_before" "$cpu_calc_after" "$spec")
            rm -f "$cpu_before" "$cpu_after" "$cpu_client_before" "$cpu_client_after"
            if [ "$measured_run" -le 0 ]; then
                [ "$run" -lt "$total_runs" ] && sleep 1
                continue
            fi
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
                fi
            fi

            [ "$run" -lt "$total_runs" ] && sleep 1
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
            printf "  %-10s %12s %8s %10s %10s %10s\n" \
                "$name" "$tp_avg" "$mbps" "$p50_avg" "$p99_avg" "$max_avg"
            printf "  %10s srv: peak_rss=%s  cpu=%s (cores %s)\n" "" \
                "$([ -n "$srv_peak_rss" ] && echo "$((srv_peak_rss / 1024))MB" || echo n/a)" \
                "$srv_cpu_last" "$spec"
            if [ "$CPU_SAMPLING" = true ]; then
                printf "  %10s cpu: %s\n" "" "$cpu_usage_last"
            fi
        else
            warn "$name: no valid output"
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

        start_server "$bin" "$port" "$workers"
        local pid="$SRV_PID"
        sleep 2

        local row_lc; row_lc="$(printf '%s' "$row_label" | tr 'A-Z' 'a-z')"
        local out="$RUN_DIR/${CUR_PROTO}-connrate-${row_lc}-${conc_lbl}-${name}.json"
        run_client "$BIN_DIR/${CUR_PROTO}-bench" connrate \
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

    # Per-protocol connection counts and payload sizes.
    IFS=',' read -ra CONNS <<< "$PROTO_CONNS"
    IFS=',' read -ra PAYLOADS <<< "$PROTO_PAYLOADS"

    kill_servers

    info "=== protocol: ${CUR_PROTO}  (port base ${PORT_BASE}) ==="
    info "servers: ${SERVERS[*]}"
    info "conns: ${CONNS[*]}  payload: ${PAYLOADS[*]}  duration: ${DURATION}s  mode: ${MODE}"
    echo ""

    # Row loop: ST always (per the mode), MT only for protocols that have it
    # (udp has no SO_REUSEPORT, so a single bound port cannot fan out).
    local rows=()
    [[ "$MODE" == "st" || "$MODE" == "both" ]] && rows+=("st")
    if [[ "$MODE" == "mt" || "$MODE" == "both" ]]; then
        if [ "$PROTO_HAS_MT" = true ]; then
            rows+=("mt")
        else
            info "[${CUR_PROTO}] no MT row for this protocol; skipping."
            echo ""
        fi
    fi

    local row row_label suffix workers
    for row in "${rows[@]}"; do
        if [ "$row" = "mt" ]; then
            row_label="MT"; suffix="-echo-mt"; workers="$SERVER_NCPU"
        else
            row_label="ST"; suffix="-echo"; workers=""
        fi
        for payload in "${PAYLOADS[@]}"; do
            for conns in "${CONNS[@]}"; do
                bench_throughput "$row_label" "$suffix" "$workers" "$conns" "$payload"
            done
        done
        if [ "$RUN_CONNRATE" = true ] && [ "$PROTO_HAS_CONNRATE" = true ]; then
            for conns in "${CONNS[@]}"; do
                bench_connrate "$row_label" "$suffix" "$workers" "$conns"
            done
        fi
    done
}

cmd_bench() {
    ensure_bin

    ulimit -n "$ULIMIT_HARD" 2>/dev/null || ulimit -n 65535 2>/dev/null || true

    local ts; ts="$(date +%Y%m%d-%H%M%S)"
    RUN_DIR="$RESULTS_ROOT/$ts"
    mkdir -p "$RUN_DIR"

    info "results -> $RUN_DIR   (MT workers = ${SERVER_NCPU})"
    info "protocols: ${PROTOS[*]}"
    if [ "$PIN_ENABLE" = true ]; then
        info "core-pinning: server cores ${SERVER_CORES} (ST: ${SERVER_CORE_ST}), client cores ${CLIENT_CORES} (GOMAXPROCS=${CLIENT_NCPU}) of ${NCPU_TOTAL}"
    else
        info "core-pinning: off (set PIN=on to enable; needs taskset and >=4 cores)"
    fi
    echo ""

    local proto
    for proto in "${PROTOS[@]}"; do
        bench_proto "$proto"
    done

    ok "benchmarks complete"
    info "results written to $RUN_DIR"
}

# =============================================================================
# fixed bench matrix
# =============================================================================

# Default benchmark matrix constants.  The CLI argument selects one protocol
# (tcp|udp|tls); with no argument every protocol runs. The matrix below is
# fixed and applies to every run. Edit the NET_BENCH_* constants to change
# the standard suite.

NET_BENCH_PROTOS="tcp,udp,tls"
NET_BENCH_SERVERS="xylem,go,rust"
NET_BENCH_CONNS="1000,10000"
NET_BENCH_PAYLOADS="64,4096,65536"
NET_BENCH_DURATION=10
NET_BENCH_MODE="both"
NET_BENCH_WARMUP_RUNS=1
NET_BENCH_RUN_CONNRATE=true

# Split the comma-separated constants into arrays.  Called after CLI-override
# has a chance to replace NET_BENCH_PROTOS.
split_bench_matrix() {
    IFS=',' read -ra PROTOS    <<< "$NET_BENCH_PROTOS"
    IFS=',' read -ra SERVERS   <<< "$NET_BENCH_SERVERS"
    IFS=',' read -ra CONNS     <<< "$NET_BENCH_CONNS"
    IFS=',' read -ra PAYLOADS  <<< "$NET_BENCH_PAYLOADS"
    DURATION="$NET_BENCH_DURATION"
    MODE="$NET_BENCH_MODE"
    BENCH_WARMUP_RUNS="$NET_BENCH_WARMUP_RUNS"
    RUN_CONNRATE="$NET_BENCH_RUN_CONNRATE"
}

parse_bench_opts() {
    split_bench_matrix

    # Validate the protocol list and fixed matrix settings.
    local p
    for p in "${PROTOS[@]}"; do proto_config "$p"; done
    if [[ "$MODE" != "st" && "$MODE" != "mt" && "$MODE" != "both" ]]; then
        err "invalid fixed mode: $MODE (must be st|mt|both)"
        exit 1
    fi
}

# =============================================================================
# dispatcher
# =============================================================================

usage() {
    cat <<EOF
usage: $0 [tcp|udp|tls|help]

Cross-platform (Linux + macOS). Current platform: $PLATFORM

Arguments:
  tcp|udp|tls   build + run the full comparison matrix for that protocol
                (xylem vs go vs rust; ST, and MT where the protocol has it)
  help          this help
  (none)        run every protocol: $NET_BENCH_PROTOS   [default]

Fixed matrix (edit NET_BENCH_* constants in this script to change defaults):
  servers:    $NET_BENCH_SERVERS
  conns:      $NET_BENCH_CONNS
  payloads:   $NET_BENCH_PAYLOADS
  duration:   ${NET_BENCH_DURATION}s
  mode:       $NET_BENCH_MODE
  connrate:   $NET_BENCH_RUN_CONNRATE

Notes:
  Missing dependencies are installed automatically at run start (Linux:
  sudo apt + rust, macOS: brew; openssl/libssl-dev only when tls is requested).
  TLS builds xylem with -DXYLEM_ENABLE_TLS=ON.
  UDP has no MT row.
  Throughput runs $NET_BENCH_WARMUP_RUNS uncounted warmup pass(es).
  macOS uses kqueue and lacks SO_REUSEPORT / /proc; numbers are NOT comparable
  to the Linux suite.

Examples:
  $0 tcp          # full TCP matrix (ST + MT, throughput + connrate)
  $0 udp          # UDP matrix (ST only)
  $0 tls          # TLS matrix (requires OpenSSL)
  $0              # all protocols
EOF
}

main() {
    # If stdout is piped, re-exec with line-buffered output for real-time visibility
    if [ ! -t 1 ] && command -v stdbuf >/dev/null 2>&1 && [ "${STDBUF_RERUN:-}" != "1" ]; then
        STDBUF_RERUN=1 exec stdbuf -oL "$0" "$@"
    fi

    local target="${1:-}"

    if [ "$#" -gt 1 ]; then
        err "unexpected extra arguments: ${*:2}"
        usage
        exit 1
    fi

    case "$target" in
        -h|--help|help) usage ;;
        ""|tcp|udp|tls)
            [ -n "$target" ] && NET_BENCH_PROTOS="$target"
            parse_bench_opts
            ensure_deps
            cmd_build
            cmd_bench
            ;;
        *)
            err "unknown target: $target (must be tcp|udp|tls|help)"
            usage
            exit 1 ;;
    esac
}

main "$@"
