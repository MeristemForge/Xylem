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

    info "building xylem sync-bench..."
    if [[ " ${PRIMS[*]} " == *" mutex "* ]]; then
        src="$SYNC_DIR/mutex/xylem/main.c"
        gcc $CFLAGS -I"$PROJECT_ROOT/include" -I"$PROJECT_ROOT/src" \
            "$src" "$XYLEM_LIB" -lpthread $LDFLAGS \
            -o "$BIN_DIR/mutex-xylem" || { err "mutex-xylem build failed"; exit 1; }
        ok "mutex-xylem built"
    fi

    if [[ " ${PRIMS[*]} " == *" cond "* ]]; then
        src="$SYNC_DIR/cond/xylem/main.c"
        gcc $CFLAGS -I"$PROJECT_ROOT/include" -I"$PROJECT_ROOT/src" \
            "$src" "$XYLEM_LIB" -lpthread $LDFLAGS \
            -o "$BIN_DIR/cond-xylem" || { err "cond-xylem build failed"; exit 1; }
        ok "cond-xylem built"
    fi

    if [[ " ${PRIMS[*]} " == *" sem "* ]]; then
        src="$SYNC_DIR/sem/xylem/main.c"
        gcc $CFLAGS -I"$PROJECT_ROOT/include" -I"$PROJECT_ROOT/src" \
            "$src" "$XYLEM_LIB" -lpthread $LDFLAGS \
            -o "$BIN_DIR/sem-xylem" || { err "sem-xylem build failed"; exit 1; }
        ok "sem-xylem built"
    fi

    if [[ " ${PRIMS[*]} " == *" channel "* ]]; then
        src="$SYNC_DIR/channel/xylem/main.c"
        gcc $CFLAGS -I"$PROJECT_ROOT/include" -I"$PROJECT_ROOT/src" \
            "$src" "$XYLEM_LIB" -lpthread $LDFLAGS \
            -o "$BIN_DIR/channel-xylem" || { err "channel-xylem build failed"; exit 1; }
        ok "channel-xylem built"
    fi

    if command -v go >/dev/null 2>&1; then
        if [[ " ${PRIMS[*]} " == *" mutex "* ]]; then
            info "building mutex-go..."
            ( cd "$SYNC_DIR/mutex/go" && CGO_ENABLED=0 go build -ldflags="-s -w" \
                -o "$BIN_DIR/mutex-go" . ) || warn "skip mutex-go (build failed)"
        fi
        if [[ " ${PRIMS[*]} " == *" cond "* ]]; then
            info "building cond-go..."
            ( cd "$SYNC_DIR/cond/go" && CGO_ENABLED=0 go build -ldflags="-s -w" \
                -o "$BIN_DIR/cond-go" . ) || warn "skip cond-go (build failed)"
        fi
        if [[ " ${PRIMS[*]} " == *" channel "* ]]; then
            info "building channel-go..."
            ( cd "$SYNC_DIR/channel/go" && CGO_ENABLED=0 go build -ldflags="-s -w" \
                -o "$BIN_DIR/channel-go" . ) || warn "skip channel-go (build failed)"
        fi
    else
        warn "go not found; skipping"
    fi

    if [[ " ${PRIMS[*]} " == *" mutex "* ]]; then
        info "building mutex-rust..."
        ( cd "$SYNC_DIR/mutex/rust" && cargo build --release -q \
            --target-dir "$BIN_DIR/cargo" && \
          cp "$BIN_DIR/cargo/release/mutex-rust" "$BIN_DIR/" ) \
          2>/dev/null && ok "mutex-rust built" || warn "skip mutex-rust"
    fi

    if [[ " ${PRIMS[*]} " == *" cond "* ]]; then
        info "building cond-rust..."
        ( cd "$SYNC_DIR/cond/rust" && cargo build --release -q \
            --target-dir "$BIN_DIR/cargo" && \
          cp "$BIN_DIR/cargo/release/cond-rust" "$BIN_DIR/" ) \
          2>/dev/null && ok "cond-rust built" || warn "skip cond-rust"
    fi

    if [[ " ${PRIMS[*]} " == *" sem "* ]]; then
        info "building sem-rust..."
        ( cd "$SYNC_DIR/sem/rust" && cargo build --release -q \
            --target-dir "$BIN_DIR/cargo" && \
          cp "$BIN_DIR/cargo/release/sem-rust" "$BIN_DIR/" ) \
          2>/dev/null && ok "sem-rust built" || warn "skip sem-rust"
    fi

    if [[ " ${PRIMS[*]} " == *" channel "* ]]; then
        info "building channel-rust..."
        ( cd "$SYNC_DIR/channel/rust" && cargo build --release -q \
            --target-dir "$BIN_DIR/cargo" && \
          cp "$BIN_DIR/cargo/release/channel-rust" "$BIN_DIR/" ) \
          2>/dev/null && ok "channel-rust built" || warn "skip channel-rust"
    fi

    echo ""
    ls -lh "$BIN_DIR"
}

# =============================================================================
# bench
# =============================================================================


bench_mutex() {
    local run_dir="$1"

    info "=== mutex  (tasks=2*workers, 5s) ==="
    printf "  %-7s %-7s %10s %10s %14s\n" \
        "LANG" "MODE" "ops/s" "ns/op" "total_ops"
    printf "  %s\n" "------------------------------------------------------"

    for lang in "${LANGS[@]}"; do
        local bin=""
        local modes=()
        case "$lang" in
            xylem) bin="$BIN_DIR/mutex-xylem"; modes=(cc tt ct tc);;
            go)    bin="$BIN_DIR/mutex-go";    modes=(cc);;
            rust)  bin="$BIN_DIR/mutex-rust";  modes=(cc tt);;
            *)     warn "skip $lang (mutex unsupported)"; continue;;
        esac
        [ -x "$bin" ] || { warn "skip $lang (no binary)"; continue; }

        for mode in "${modes[@]}"; do
            local out="$run_dir/sync-mutex-${lang}-${mode}.json"
            "$bin" > "$out" 2>/dev/null || true

            local ops="" nspo="" total=""
            if [ -s "$out" ]; then
                local block
                block=$(awk -v m="$mode" '
                    /^{/ { in_obj=1; buf=$0; next }
                    in_obj { buf=buf "\n" $0 }
                    /^}/ {
                        if (buf ~ "\"mode\": \"" m "\"") print buf
                        in_obj=0; buf=""
                    }' "$out")
                if [ -n "$block" ]; then
                    ops=$(echo "$block" | grep "\"ops_per_sec\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    nspo=$(echo "$block" | grep "\"ns_per_op\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    total=$(echo "$block" | grep "\"total_ops\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    ops=${ops%%.*}
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

bench_cond() {
    local run_dir="$1"

    info "=== cond  (ping-pong, 5s) ==="
    printf "  %-7s %-7s %10s %10s %14s\n" \
        "LANG" "MODE" "ops/s" "ns/op" "total_ops"
    printf "  %s\n" "------------------------------------------------------"

    for lang in "${LANGS[@]}"; do
        local bin=""
        local modes=()
        case "$lang" in
            xylem) bin="$BIN_DIR/cond-xylem"; modes=(cc tt ct tc);;
            go)    bin="$BIN_DIR/cond-go";    modes=(cc);;
            rust)  bin="$BIN_DIR/cond-rust";  modes=(tt);;
            *) warn "skip $lang (cond unsupported)"; continue;;
        esac

        [ -x "$bin" ] || { warn "skip $lang (no binary)"; continue; }

        for mode in "${modes[@]}"; do
            local out="$run_dir/sync-cond-${lang}-${mode}.json"
            "$bin" > "$out" 2>/dev/null || true

            local ops="" nspo="" total=""
            if [ -s "$out" ]; then
                local block
                block=$(awk -v m="$mode" '
                    /^{/ { in_obj=1; buf=$0; next }
                    in_obj { buf=buf "\n" $0 }
                    /^}/ {
                        if (buf ~ "\"mode\": \"" m "\"") print buf
                        in_obj=0; buf=""
                    }' "$out")
                if [ -n "$block" ]; then
                    ops=$(echo "$block" | grep "\"ops_per_sec\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    nspo=$(echo "$block" | grep "\"ns_per_op\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    total=$(echo "$block" | grep "\"total_ops\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    ops=${ops%%.*}
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

bench_sem() {
    local run_dir="$1"

    info "=== sem  (handoff, 5s) ==="
    printf "  %-7s %-7s %10s %10s %14s\n" \
        "LANG" "MODE" "ops/s" "ns/op" "total_ops"
    printf "  %s\n" "------------------------------------------------------"

    for lang in "${LANGS[@]}"; do
        local bin="" modes=()
        case "$lang" in
            xylem) bin="$BIN_DIR/sem-xylem"; modes=(cc tt ct tc);;
            go)    warn "skip go (sem unsupported)"; continue;;
            rust)  bin="$BIN_DIR/sem-rust";  modes=(coro);;
            *)     warn "skip $lang (sem unsupported)"; continue;;
        esac
        [ -x "$bin" ] || { warn "skip $lang (no binary)"; continue; }

        for mode in "${modes[@]}"; do
            local out="$run_dir/sync-sem-${lang}-${mode}.json"
            "$bin" > "$out" 2>/dev/null || true

            local ops="" nspo="" total=""
            if [ -s "$out" ]; then
                local block
                block=$(awk -v m="$mode" '
                    /^{/ { in_obj=1; buf=$0; next }
                    in_obj { buf=buf "\n" $0 }
                    /^}/ {
                        if (buf ~ "\"mode\": \"" m "\"") print buf
                        in_obj=0; buf=""
                    }' "$out")
                if [ -n "$block" ]; then
                    ops=$(echo "$block" | grep "\"ops_per_sec\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    nspo=$(echo "$block" | grep "\"ns_per_op\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    total=$(echo "$block" | grep "\"total_ops\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    ops=${ops%%.*}
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

bench_channel() {
    local run_dir="$1"

    info "=== channel  (one-way, 5s) ==="
    printf "  %-7s %-7s %10s %10s %14s\n" \
        "LANG" "MODE" "ops/s" "ns/op" "total_ops"
    printf "  %s\n" "------------------------------------------------------"

    for lang in "${LANGS[@]}"; do
        local bin=""
        local modes=()
        case "$lang" in
            xylem) bin="$BIN_DIR/channel-xylem"; modes=(cc tt ct tc);;
            go)    bin="$BIN_DIR/channel-go";    modes=(cc);;
            rust)  bin="$BIN_DIR/channel-rust";  modes=(cc tt ct tc);;
            *) warn "skip $lang (channel unsupported)"; continue;;
        esac

        [ -x "$bin" ] || { warn "skip $lang (no binary)"; continue; }

        for mode in "${modes[@]}"; do
            local out="$run_dir/sync-channel-${lang}-${mode}.json"
            "$bin" > "$out" 2>/dev/null || true

            local ops="" nspo="" total=""
            if [ -s "$out" ]; then
                local block
                block=$(awk -v m="$mode" '
                    /^{/ { in_obj=1; buf=$0; next }
                    in_obj { buf=buf "\n" $0 }
                    /^}/ {
                        if (buf ~ "\"mode\": \"" m "\"") print buf
                        in_obj=0; buf=""
                    }' "$out")
                if [ -n "$block" ]; then
                    ops=$(echo "$block" | grep "\"ops_per_sec\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    nspo=$(echo "$block" | grep "\"ns_per_op\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    total=$(echo "$block" | grep "\"total_ops\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                    ops=${ops%%.*}
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

    for prim in "${PRIMS[@]}"; do
        if [ "$prim" = "mutex" ]; then
            bench_mutex "$run_dir"
        elif [ "$prim" = "cond" ]; then
            bench_cond "$run_dir"
        elif [ "$prim" = "sem" ]; then
            bench_sem "$run_dir"
        elif [ "$prim" = "channel" ]; then
            bench_channel "$run_dir"
        fi
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
