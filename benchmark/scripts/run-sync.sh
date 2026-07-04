#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Xylem sync-primitive benchmark (CC only: coroutine / goroutine / task)
# -----------------------------------------------------------------------------
#   build  - build xylem static lib + C/Go/Rust sync-bench binaries
#   bench  - run every primitive across xylem/go/rust, write out/results/<ts>/
#   all    - build + bench                                          [default]
#
# Primitives (--prims, comma-separated): mutex,cond,waitgroup,sem,channel
# Languages  (--langs, comma-separated): xylem,go,rust
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

if [ "$(uname -s)" = "Darwin" ]; then ncpu() { sysctl -n hw.ncpu; }
else ncpu() { nproc; }; fi

IFS=',' read -ra PRIMS <<< "${PRIMS:-mutex,cond,waitgroup,sem,channel}"
IFS=',' read -ra LANGS <<< "${LANGS:-xylem,go,rust}"
WORKERS="${WORKERS:-0}"
REPEAT="${REPEAT:-3}"

declare -A P_TASKS=( [mutex]=8 [cond]=2 [waitgroup]=8 [channel]=4 )
declare -A P_ITERS=( [mutex]=1000000 [cond]=1000 [waitgroup]=1 [channel]=1000000 )
P_PERMITS="${PERMITS:-4}"

CFLAGS="-std=gnu11 -O3 -DNDEBUG -flto -Wall -Wextra"
LDFLAGS="-s -flto"

# =============================================================================
# build
# =============================================================================

cmd_build() {
    mkdir -p "$BIN_DIR"

    if ! command -v cmake >/dev/null 2>&1; then
        err "cmake not found"; exit 1
    fi
    if ! command -v ninja >/dev/null 2>&1; then
        err "ninja not found"; exit 1
    fi

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
        if [ -d "$SYNC_DIR/go-sync" ]; then
            info "building go sync-bench..."
            ( cd "$SYNC_DIR/go-sync" && CGO_ENABLED=0 go build -ldflags="-s -w" \
                -o "$BIN_DIR/sync-go" . ) || warn "skip go (build failed)"
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

    if command -v cargo >/dev/null 2>&1; then
        if [ -d "$SYNC_DIR/rust-sync" ]; then
            info "building rust sync-bench..."
            ( cd "$SYNC_DIR/rust-sync" && cargo build --release -q \
                --target-dir "$BIN_DIR/cargo" && \
              cp "$BIN_DIR/cargo/release/sync-rust" "$BIN_DIR/" && \
              strip "$BIN_DIR/sync-rust" ) \
              || warn "skip rust (build failed)"
        fi
    else
        warn "cargo not found; skipping"
    fi

    echo ""
    ls -lh "$BIN_DIR"
}

# =============================================================================
# bench
# =============================================================================

bin_for() {
    case "$1" in
        xylem) echo "$BIN_DIR/sync-xylem";;
        go)    echo "$BIN_DIR/sync-go";;
        rust)  echo "$BIN_DIR/sync-rust";;
    esac
}

extract_json() {
    grep "\"$2\"" "$1" 2>/dev/null | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1
}

bench_mutex() {
    local run_dir="$1"

    info "=== mutex  (tasks=2*workers, 5s) ==="
    printf "  %-7s %-7s %10s %10s %14s  %s\n" \
        "LANG" "MODE" "ops/s(avg)" "ns/op" "total_ops" "runs(ops/s)"
    printf "  %s\n" "-----------------------------------------------------------------"

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
            local ops_sum=0 nspo_sum=0 total_last=0 valid=0 ops_vals=""
            for run in $(seq 1 "$REPEAT"); do
                local out="$run_dir/sync-mutex-${lang}-r${run}.json"
                [ -s "$out" ] || "$bin" > "$out" 2>/dev/null || true

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
                        local ops nspo total
                        ops=$(echo "$block" | grep "\"ops_per_sec\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        nspo=$(echo "$block" | grep "\"ns_per_op\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        total=$(echo "$block" | grep "\"total_ops\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        ops=${ops%%.*}
                        if [ -n "$ops" ] && [ "$ops" -gt 0 ] 2>/dev/null; then
                            ops_sum=$((ops_sum + ops))
                            nspo_sum=$(awk -v a="$nspo_sum" -v b="$nspo" 'BEGIN { printf "%.6f", a + b }')
                            total_last="$total"
                            valid=$((valid + 1))
                            ops_vals="${ops_vals:+$ops_vals,}$ops"
                        fi
                    fi
                fi
            done

            if [ "$valid" -gt 0 ]; then
                local ops_avg=$((ops_sum / valid))
                local nspo_avg; nspo_avg=$(awk -v s="$nspo_sum" -v n="$valid" 'BEGIN { printf "%.2f", s / n }')
                printf "  %-7s %-7s %10s %10s %14s  [%s]\n" \
                    "$lang" "$mode" "$ops_avg" "$nspo_avg" "$total_last" "$ops_vals"
            else
                warn "$lang/$mode: no valid output from $REPEAT runs"
            fi
        done
    done
    echo ""
}

bench_cond() {
    local run_dir="$1"

    info "=== cond  (ping-pong, 5s) ==="
    printf "  %-7s %-7s %10s %10s %14s  %s\n" \
        "LANG" "MODE" "ops/s(avg)" "ns/op" "total_ops" "runs(ops/s)"
    printf "  %s\n" "-----------------------------------------------------------------"

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
            local ops_sum=0 nspo_sum=0 total_last=0 valid=0 ops_vals=""
            for run in $(seq 1 "$REPEAT"); do
                local out="$run_dir/sync-cond-${lang}-${mode}-r${run}.json"
                "$bin" > "$out" 2>/dev/null || true

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
                        local ops nspo total
                        ops=$(echo "$block" | grep "\"ops_per_sec\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        nspo=$(echo "$block" | grep "\"ns_per_op\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        total=$(echo "$block" | grep "\"total_ops\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        ops=${ops%%.*}
                        if [ -n "$ops" ] && [ "$ops" -gt 0 ] 2>/dev/null; then
                            ops_sum=$((ops_sum + ops))
                            nspo_sum=$(awk -v a="$nspo_sum" -v b="$nspo" 'BEGIN { printf "%.6f", a + b }')
                            total_last="$total"
                            valid=$((valid + 1))
                            ops_vals="${ops_vals:+$ops_vals,}$ops"
                        fi
                    fi
                fi
            done

            if [ "$valid" -gt 0 ]; then
                local ops_avg=$((ops_sum / valid))
                local nspo_avg; nspo_avg=$(awk -v s="$nspo_sum" -v n="$valid" 'BEGIN { printf "%.2f", s / n }')
                printf "  %-7s %-7s %10s %10s %14s  [%s]\n" \
                    "$lang" "$mode" "$ops_avg" "$nspo_avg" "$total_last" "$ops_vals"
            else
                warn "$lang/$mode: no valid output from $REPEAT runs"
            fi
        done
    done
    echo ""
}

bench_sem() {
    local run_dir="$1"

    info "=== sem  (handoff, 5s) ==="
    printf "  %-7s %-7s %10s %10s %14s  %s\n" \
        "LANG" "MODE" "ops/s(avg)" "ns/op" "total_ops" "runs(ops/s)"
    printf "  %s\n" "-----------------------------------------------------------------"

    for lang in "${LANGS[@]}"; do
        local bin; bin="$(bin_for "$lang")"
        [ -x "$bin" ] || { warn "skip $lang (no binary)"; continue; }
        local modes=()
        case "$lang" in
            xylem) modes=(cc tt ct tc);;
            rust)  modes=(coro);;
        esac

        for mode in "${modes[@]}"; do
            local ops_sum=0 nspo_sum=0 total_last=0 valid=0 ops_vals=""
            for run in $(seq 1 "$REPEAT"); do
                local out="$run_dir/sync-sem-${lang}-${mode}-r${run}.json"
                if [ "$lang" = "xylem" ]; then
                    "$BIN_DIR/sem-xylem" > "$out" 2>/dev/null || true
                else
                    "$BIN_DIR/sem-rust" > "$out" 2>/dev/null || true
                fi

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
                        local ops nspo total
                        ops=$(echo "$block" | grep "\"ops_per_sec\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        nspo=$(echo "$block" | grep "\"ns_per_op\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        total=$(echo "$block" | grep "\"total_ops\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        ops=${ops%%.*}
                        if [ -n "$ops" ] && [ "$ops" -gt 0 ] 2>/dev/null; then
                            ops_sum=$((ops_sum + ops))
                            nspo_sum=$(awk -v a="$nspo_sum" -v b="$nspo" 'BEGIN { printf "%.6f", a + b }')
                            total_last="$total"
                            valid=$((valid + 1))
                            ops_vals="${ops_vals:+$ops_vals,}$ops"
                        fi
                    fi
                fi
            done

            if [ "$valid" -gt 0 ]; then
                local ops_avg=$((ops_sum / valid))
                local nspo_avg; nspo_avg=$(awk -v s="$nspo_sum" -v n="$valid" 'BEGIN { printf "%.2f", s / n }')
                printf "  %-7s %-7s %10s %10s %14s  [%s]\n" \
                    "$lang" "$mode" "$ops_avg" "$nspo_avg" "$total_last" "$ops_vals"
            else
                warn "$lang/$mode: no valid output from $REPEAT runs"
            fi
        done
    done
    echo ""
}

bench_channel() {
    local run_dir="$1"

    info "=== channel  (one-way, 5s) ==="
    printf "  %-7s %-7s %10s %10s %14s  %s\n" \
        "LANG" "MODE" "ops/s(avg)" "ns/op" "total_ops" "runs(ops/s)"
    printf "  %s\n" "-----------------------------------------------------------------"

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
            local ops_sum=0 nspo_sum=0 total_last=0 valid=0 ops_vals=""
            for run in $(seq 1 "$REPEAT"); do
                local out="$run_dir/sync-channel-${lang}-${mode}-r${run}.json"
                "$bin" > "$out" 2>/dev/null || true

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
                        local ops nspo total
                        ops=$(echo "$block" | grep "\"ops_per_sec\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        nspo=$(echo "$block" | grep "\"ns_per_op\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        total=$(echo "$block" | grep "\"total_ops\"" | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1)
                        ops=${ops%%.*}
                        if [ -n "$ops" ] && [ "$ops" -gt 0 ] 2>/dev/null; then
                            ops_sum=$((ops_sum + ops))
                            nspo_sum=$(awk -v a="$nspo_sum" -v b="$nspo" 'BEGIN { printf "%.6f", a + b }')
                            total_last="$total"
                            valid=$((valid + 1))
                            ops_vals="${ops_vals:+$ops_vals,}$ops"
                        fi
                    fi
                fi
            done

            if [ "$valid" -gt 0 ]; then
                local ops_avg=$((ops_sum / valid))
                local nspo_avg; nspo_avg=$(awk -v s="$nspo_sum" -v n="$valid" 'BEGIN { printf "%.2f", s / n }')
                printf "  %-7s %-7s %10s %10s %14s  [%s]\n" \
                    "$lang" "$mode" "$ops_avg" "$nspo_avg" "$total_last" "$ops_vals"
            else
                warn "$lang/$mode: no valid output from $REPEAT runs"
            fi
        done
    done
    echo ""
}

cmd_bench() {
    local ts; ts="$(date +%Y%m%d-%H%M%S)"
    local run_dir="$RESULTS_ROOT/$ts"
    mkdir -p "$run_dir"

    info "results -> $run_dir   repeat=${REPEAT}"
    info "prims: ${PRIMS[*]}   langs: ${LANGS[*]}"
    echo ""

    for prim in "${PRIMS[@]}"; do
        if [ "$prim" = "mutex" ]; then
            bench_mutex "$run_dir"
            continue
        fi
        if [ "$prim" = "cond" ]; then
            bench_cond "$run_dir"
            continue
        fi
        if [ "$prim" = "sem" ]; then
            bench_sem "$run_dir"
            continue
        fi
        if [ "$prim" = "channel" ]; then
            bench_channel "$run_dir"
            continue
        fi

        local tasks="${P_TASKS[$prim]}"
        info "=== ${prim}  (tasks=${tasks}) ==="
        printf "  %-7s %10s %10s %14s  %s\n" \
            "LANG" "ops/s(avg)" "ns/op" "total_ops" "runs(ops/s)"
        printf "  %s\n" "---------------------------------------------------------------------"

        for lang in "${LANGS[@]}"; do
            local bin; bin="$(bin_for "$lang")"
            [ -x "$bin" ] || { warn "skip $lang (no binary)"; continue; }

            local iters="${P_ITERS[$prim]}"
            local args=(--tasks "$tasks" --iters "$iters" --workers "$WORKERS")

            local ops_sum=0 nspo_sum=0 total_last=0 valid=0 ops_vals=""
            for run in $(seq 1 "$REPEAT"); do
                local out="$run_dir/sync-${prim}-${lang}-r${run}.json"
                "$bin" "$prim" "${args[@]}" > "$out" 2>/dev/null || true
                if [ -s "$out" ]; then
                    local ops nspo total
                    ops=$(extract_json "$out" ops_per_sec)
                    nspo=$(extract_json "$out" ns_per_op)
                    total=$(extract_json "$out" total_ops)
                    ops=${ops%%.*}
                    if [ -n "$ops" ] && [ "$ops" -gt 0 ]; then
                        ops_sum=$((ops_sum + ops))
                        nspo_sum=$(awk -v a="$nspo_sum" -v b="$nspo" 'BEGIN { printf "%.6f", a + b }')
                        total_last="$total"
                        valid=$((valid + 1))
                        ops_vals="${ops_vals:+$ops_vals,}$ops"
                    fi
                fi
            done

            if [ "$valid" -gt 0 ]; then
                local ops_avg=$((ops_sum / valid))
                local nspo_avg; nspo_avg=$(awk -v s="$nspo_sum" -v n="$valid" 'BEGIN { printf "%.2f", s / n }')
                printf "  %-7s %10s %10s %14s  [%s]\n" \
                    "$lang" "$ops_avg" "$nspo_avg" "$total_last" "$ops_vals"
            else
                warn "$lang: no valid output from $REPEAT runs"
            fi
        done
        echo ""
    done

    ok "sync benchmarks complete"
    info "results written to $run_dir"
}

# =============================================================================
# dispatcher
# =============================================================================

usage() {
    cat <<EOF
usage: $0 [build|bench|all] [options...]

Commands:
  build     xylem static lib + per-language sync-bench binaries
  bench     run comparison benchmarks, write benchmark/out/results/<ts>/
  all       build + bench   (default)

Options (env vars seed defaults):
  --prims, -p    mutex,cond,waitgroup,sem,channel   (default: all)
  --langs, -l    xylem,go,rust                       (default: all)
  --repeat, -r   3                                   repeat each test N times
  --workers, -w  0                                   scheduler workers (0=auto)

Examples:
  $0 all --prims mutex,sem --langs xylem,go
EOF
}

parse_opts() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --prims|-p) shift; IFS=',' read -ra PRIMS <<< "$1" ;;
            --langs|-l) shift; IFS=',' read -ra LANGS <<< "$1" ;;
            --repeat|-r) shift; REPEAT="$1" ;;
            --workers|-w) shift; WORKERS="$1" ;;
            *) err "unknown option: $1"; usage; exit 1 ;;
        esac
        shift
    done
}

main() {
    local cmd="${1:-all}"
    shift || true

    case "$cmd" in
        build) parse_opts "$@"; cmd_build ;;
        bench) parse_opts "$@"; cmd_bench ;;
        all)   parse_opts "$@"; cmd_build; cmd_bench ;;
        -h|--help|help) usage ;;
        *) err "unknown command: $cmd"; usage; exit 1 ;;
    esac
}

main "$@"
