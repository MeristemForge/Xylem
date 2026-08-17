#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Xylem sync-primitive benchmark (per-primitive, fixed-duration)
# -----------------------------------------------------------------------------
#   mutex|cond|sem|channel - build + run the full comparison matrix for that
#                            primitive (xylem vs go vs rust, all supported
#                            modes)
#   (no argument)         - every primitive                     [default]
#
# Fixed matrix -- no options; edit the constants below to change the suite:
#   prims: mutex,cond,sem,channel   langs: xylem,go,rust
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SYNC_DIR="$BENCH_DIR/sync"
PROJECT_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
OUT_DIR="$BENCH_DIR/out"
BIN_DIR="$OUT_DIR"
BUILD_DIR="$OUT_DIR/build"
RESULTS_ROOT="$OUT_DIR/results"

info() { printf "\033[1;34m[sync]\033[0m %s\n" "$1"; }
ok()   { printf "\033[1;32m[ok]\033[0m %s\n" "$1"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$1"; }
err()  { printf "\033[1;31m[err]\033[0m %s\n" "$1" >&2; }

if [ "$(uname -s)" = "Darwin" ]; then
    PLATFORM="macos"
    ncpu() { sysctl -n hw.ncpu; }
else
    PLATFORM="linux"
    ncpu() { nproc; }
fi

# Fixed matrix (no CLI options -- edit these constants to change the suite).
PRIMS=(mutex cond sem channel)
LANGS=(xylem go rust)

CFLAGS="-std=gnu11 -O3 -DNDEBUG -flto -Wall -Wextra"
LDFLAGS="-s -flto"

# =============================================================================
# dependencies (auto-installed at run time when missing)
# =============================================================================

# Fill MISSING_DEPS with anything not installed. Tools are probed with
# command -v so installs via brew, official installers, or rustup are all
# recognized; Linux additionally needs a C toolchain (build-essential).
find_missing_deps() {
    MISSING_DEPS=()
    local tool
    for tool in cmake ninja go; do
        command -v "$tool" >/dev/null 2>&1 || MISSING_DEPS+=("$tool")
    done
    command -v cargo >/dev/null 2>&1 || MISSING_DEPS+=("rust")
    if [ "$PLATFORM" != "macos" ]; then
        dpkg -s build-essential >/dev/null 2>&1 || MISSING_DEPS+=("build-essential")
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

cmd_build() {
    mkdir -p "$BIN_DIR"

    info "building xylem static library..."
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS='-O3 -DNDEBUG -flto' \
        -DXYLEM_ENABLE_TLS=OFF \
        -G Ninja
    ninja -C "$BUILD_DIR" xylem -j"$(ncpu)"
    XYLEM_LIB="$BUILD_DIR/libxylem.a"
    ok "xylem built"

    # One directory per primitive; each has xylem (always) plus go and rust
    # where the language has that primitive (sem has no go implementation).
    local prim
    for prim in "${PRIMS[@]}"; do
        info "building ${prim}-xylem..."
        gcc $CFLAGS -I"$PROJECT_ROOT/include" -I"$PROJECT_ROOT/src" \
            "$SYNC_DIR/$prim/xylem/main.c" "$XYLEM_LIB" -lpthread $LDFLAGS \
            -o "$BIN_DIR/$prim-xylem" \
            || { err "$prim-xylem build failed"; exit 1; }
        ok "$prim-xylem built"

        local godir="$SYNC_DIR/$prim/go"
        if [ -d "$godir" ] && command -v go >/dev/null 2>&1; then
            info "building ${prim}-go..."
            ( cd "$godir" && CGO_ENABLED=0 go build -ldflags="-s -w" \
                -o "$BIN_DIR/$prim-go" . ) || warn "skip $prim-go (build failed)"
        fi

        local rustdir="$SYNC_DIR/$prim/rust"
        if [ -d "$rustdir" ]; then
            info "building ${prim}-rust..."
            ( cd "$rustdir" && cargo build --release -q \
                --target-dir "$BIN_DIR/cargo" && \
              cp "$BIN_DIR/cargo/release/$prim-rust" "$BIN_DIR/" ) \
              2>/dev/null && ok "$prim-rust built" || warn "skip $prim-rust"
        fi
    done

    echo ""
    ls -lh "$BIN_DIR"
}

# =============================================================================
# bench
# =============================================================================


# The four primitives share one bench loop; only the header label and the
# per-language mode lists differ. The mode lists are bash-3.2-safe case tables
# (no associative arrays) keyed by "prim:lang"; returning 1 means the language
# has no implementation of that primitive.
prim_label() {
    case "$1" in
        mutex)   echo "tasks=2*workers, 5s" ;;
        cond)    echo "ping-pong, 5s" ;;
        sem)     echo "handoff, 5s" ;;
        channel) echo "one-way, 5s" ;;
    esac
}

prim_modes() {
    case "$1:$2" in
        mutex:xylem|cond:xylem|sem:xylem|channel:xylem) echo "cc tt ct tc" ;;
        mutex:go|cond:go|channel:go) echo "cc" ;;
        mutex:rust|channel:rust) echo "cc tt" ;;
        cond:rust) echo "tt" ;;
        sem:rust) echo "coro" ;;
        *) return 1 ;;
    esac
}

# Extract the JSON object whose "mode" matches $2 from a result file. The
# binaries emit one flat object per mode, with braces only at line starts.
extract_mode_block() {
    local file="$1" mode="$2"
    awk -v m="$mode" '
        /^{/ { in_obj=1; buf=$0; next }
        in_obj { buf=buf "\n" $0 }
        /^}/ {
            if (buf ~ "\"mode\": \"" m "\"") print buf
            in_obj=0; buf=""
        }' "$file"
}

bench_prim() {
    local prim="$1" run_dir="$2"

    info "=== ${prim}  ($(prim_label "$prim")) ==="
    printf "  %-7s %-7s %10s %10s %14s\n" \
        "LANG" "MODE" "ops/s" "ns/op" "total_ops"
    printf "  %s\n" "------------------------------------------------------"

    for lang in "${LANGS[@]}"; do
        local modes
        modes="$(prim_modes "$prim" "$lang")" || {
            warn "skip $lang ($prim unsupported)"
            continue
        }
        local bin="$BIN_DIR/${prim}-${lang}"
        [ -x "$bin" ] || { warn "skip $lang (no binary)"; continue; }

        for mode in $modes; do
            local out="$run_dir/sync-${prim}-${lang}-${mode}.json"
            "$bin" > "$out" 2>/dev/null || true

            local ops="" nspo="" total=""
            if [ -s "$out" ]; then
                local block
                block="$(extract_mode_block "$out" "$mode")"
                if [ -n "$block" ]; then
                    # One awk pass over the block extracts all three numbers
                    # (each is the only digit run on its line), replacing the
                    # three grep|grep|tail pipelines.
                    read -r ops nspo total < <(printf '%s\n' "$block" | awk '
                        /"ops_per_sec"/ { if (match($0, /[0-9]+([.][0-9]+)?/)) o = substr($0, RSTART, RLENGTH) }
                        /"ns_per_op"/  { if (match($0, /[0-9]+([.][0-9]+)?/)) n = substr($0, RSTART, RLENGTH) }
                        /"total_ops"/  { if (match($0, /[0-9]+([.][0-9]+)?/)) t = substr($0, RSTART, RLENGTH) }
                        END { print o, n, t }') || true
                    ops="${ops%%.*}"
                fi
            fi
            if [ -n "$ops" ] && [ "$ops" -gt 0 ] 2>/dev/null; then
                printf "  %-7s %-7s %10s %10s %14s\n" \
                    "$lang" "$mode" "$ops" "$nspo" "$total"
            else
                warn "$lang/$mode: no valid output"
            fi
        done
    done
    echo ""
}

cmd_bench() {
    local ts; ts="$(date +%Y%m%d-%H%M%S)"
    local run_dir="$RESULTS_ROOT/$ts"
    mkdir -p "$run_dir"

    info "results -> $run_dir"
    info "prims: ${PRIMS[*]}   langs: ${LANGS[*]}"
    echo ""

    local prim
    for prim in "${PRIMS[@]}"; do
        bench_prim "$prim" "$run_dir"
    done

    ok "sync benchmarks complete"
    info "results written to $run_dir"
}

# =============================================================================
# dispatcher
# =============================================================================

usage() {
    cat <<EOF
usage: $0 [mutex|cond|sem|channel|help]

Arguments:
  mutex|cond|sem|channel   build + run the full comparison matrix for that
                           primitive (xylem vs go vs rust, all supported modes)
  help                     this help
  (none)                   every primitive   [default]

The matrix is fixed: langs=xylem,go,rust, each cell runs once (no repeat).
Edit the constants at the top of this script to change it.
EOF
}

main() {
    # stdbuf re-exec guard: when stdout is not a tty (piped/redirected),
    # re-exec under stdbuf -oL so progress lines flush as they are written.
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
        ""|mutex|cond|sem|channel)
            [ -n "$target" ] && PRIMS=("$target")
            ensure_deps
            cmd_build
            cmd_bench
            ;;
        *)
            err "unknown target: $target (must be mutex|cond|sem|channel|help)"
            usage
            exit 1 ;;
    esac
}

main "$@"
