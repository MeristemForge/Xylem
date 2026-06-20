#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Xylem sync-primitive benchmark (Linux + macOS)
# -----------------------------------------------------------------------------
#   build  - build the xylem static lib + the C/Go/Rust sync-bench binaries
#   bench  - run each primitive across xylem/go/rust, write out/results/<ts>/
#   all    - build + bench                                          [default]
#
# Primitives (--prims, comma-separated): mutex,cond,waitgroup,sem,channel
# Languages  (--langs, comma-separated): xylem,go,rust
#
# Each binary runs one primitive and prints a JSON result; this driver runs
# every (primitive x language) cell, repeats it, averages ops/sec and prints
# a comparison table. Numbers are only comparable within one platform/run.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SYNC_DIR="$BENCH_DIR/sync"
PROJECT_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
OUT_DIR="$BENCH_DIR/out"           # shared with the net suite
BIN_DIR="$OUT_DIR"                 # compiled binaries go straight into out/
BUILD_DIR="$OUT_DIR/build"         # xylem CMake build tree
RESULTS_ROOT="$OUT_DIR/results"

info() { printf "\033[1;34m[sync]\033[0m %s\n" "$1"; }
ok()   { printf "\033[1;32m[ok]\033[0m %s\n" "$1"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$1"; }
err()  { printf "\033[1;31m[err]\033[0m %s\n" "$1" >&2; }

if [ "$(uname -s)" = "Darwin" ]; then ncpu() { sysctl -n hw.ncpu; }
else ncpu() { nproc; }; fi

# ---- defaults (env seeds; CLI overrides) -----------------------------------
IFS=',' read -ra PRIMS <<< "${PRIMS:-mutex,cond,waitgroup,sem,channel,handoff}"
IFS=',' read -ra LANGS <<< "${LANGS:-xylem,go,rust}"
IFS=',' read -ra MODES <<< "${MODES:-coro,thread,mixed}"
WORKERS="${WORKERS:-0}"          # 0 = each runtime's default (CPU count)
REPEAT="${REPEAT:-3}"

# per-primitive workload (tasks/iters/permits); sized for ~1-2s per cell
declare -A P_TASKS=( [mutex]=8 [cond]=2 [waitgroup]=8 [sem]=8 [channel]=4 [handoff]=2 )
declare -A P_ITERS=( [mutex]=1000000 [cond]=2000000 [waitgroup]=50000 [sem]=1000000 [channel]=1000000 [handoff]=500000 )
# Spawning an OS thread per unit is far costlier than a coroutine, so the
# thread/mixed modes use lighter per-primitive iteration counts.
declare -A PT_ITERS=( [mutex]=1000000 [cond]=2000000 [waitgroup]=2000 [sem]=1000000 [channel]=1000000 [handoff]=500000 )
P_PERMITS="${PERMITS:-4}"

# Support matrix: which (lang, mode) cells are valid. The binaries also reject
# unsupported combinations (non-zero exit), so this is just to avoid noise.
#   go   -> coro only;  rust -> coro,thread;  xylem -> coro,thread,mixed
# Exceptions for rust:
#   - handoff is cross-context by design, so rust supports all three modes
#     (coro=task<->task, thread=thread<->thread, mixed=external thread<->task).
#   - channel mixed works too: a channel's producer end is callable across the
#     coro/thread boundary, so senders in one context feed a receiver in the
#     other over one shared MPSC (lock-style primitives still can't, so
#     mutex/cond/sem/waitgroup remain coro,thread only).
supported() {
    local lang="$1" mode="$2" prim="$3"
    case "$lang" in
        go)    [ "$mode" = "coro" ] ;;
        rust)  if [ "$prim" = "handoff" ]; then return 0; fi
               if [ "$prim" = "channel" ] && [ "$mode" = "mixed" ]; then return 0; fi
               [ "$mode" = "coro" ] || [ "$mode" = "thread" ] ;;
        xylem) : ;;   # all modes
        *)     return 1 ;;
    esac
}

# iters for (prim, mode): thread/mixed use the lighter PT_ITERS table.
iters_for() {
    local prim="$1" mode="$2"
    if [ "$mode" = "coro" ]; then echo "${P_ITERS[$prim]}"; else echo "${PT_ITERS[$prim]}"; fi
}

CFLAGS="-std=gnu11 -O3 -DNDEBUG -flto -Wall -Wextra"
LDFLAGS="-s -flto"

# =============================================================================
# build
# =============================================================================
cmd_build() {
    mkdir -p "$BIN_DIR"

    info "building xylem static library..."
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS='-O3 -DNDEBUG -flto' \
        -G Ninja >/dev/null
    ninja -C "$BUILD_DIR" xylem -j"$(ncpu)" >/dev/null
    local xylem_lib="$BUILD_DIR/libxylem.a"
    ok "xylem built"

    local want_xylem=false want_go=false want_rust=false
    for l in "${LANGS[@]}"; do
        case "$l" in xylem) want_xylem=true;; go) want_go=true;; rust) want_rust=true;; esac
    done

    if [ "$want_xylem" = true ]; then
        info "building xylem sync-bench..."
        rm -f "$BIN_DIR/sync-xylem"
        # shellcheck disable=SC2086
        gcc $CFLAGS -I"$PROJECT_ROOT/include" \
            "$SYNC_DIR/xylem-sync/main.c" "$xylem_lib" -lpthread $LDFLAGS \
            -o "$BIN_DIR/sync-xylem" \
            && ok "sync-xylem built" || { err "sync-xylem build failed"; return 1; }
    fi

    if [ "$want_go" = true ]; then
        rm -f "$BIN_DIR/sync-go"
        if command -v go >/dev/null 2>&1; then
            info "building go sync-bench..."
            ( cd "$SYNC_DIR/go-sync" && \
              CGO_ENABLED=0 go build -ldflags="-s -w" -o "$BIN_DIR/sync-go" . ) \
              && ok "sync-go built" || { err "sync-go build failed"; return 1; }
        else
            err "go not found"
            return 1
        fi
    fi

    if [ "$want_rust" = true ]; then
        rm -f "$BIN_DIR/sync-rust"
        if command -v cargo >/dev/null 2>&1; then
            info "building rust sync-bench..."
            ( cd "$SYNC_DIR/rust-sync" && \
              cargo build --release -q && \
              cp "target/release/sync-rust" "$BIN_DIR/" && \
              strip "$BIN_DIR/sync-rust" ) \
              && ok "sync-rust built" || { err "sync-rust build failed"; return 1; }
        else
            err "cargo not found"
            return 1
        fi
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

cmd_bench() {
    local missing=false
    for l in "${LANGS[@]}"; do
        [ -x "$(bin_for "$l")" ] || { warn "binary for $l missing"; missing=true; }
    done
    [ "$missing" = true ] && { err "run: $0 build"; exit 1; }

    local ts; ts="$(date +%Y%m%d-%H%M%S)"
    local run_dir="$RESULTS_ROOT/$ts"
    mkdir -p "$run_dir"

    info "results -> $run_dir   workers=${WORKERS} repeat=${REPEAT}"
    info "prims: ${PRIMS[*]}   langs: ${LANGS[*]}   modes: ${MODES[*]}"
    echo ""

    for prim in "${PRIMS[@]}"; do
        local tasks="${P_TASKS[$prim]}"
        info "=== ${prim}  (tasks=${tasks}$([ "$prim" = sem ] && echo " permits=${P_PERMITS}")) ==="
        printf "  %-7s %-7s %16s %12s %14s  %s\n" \
            "LANG" "MODE" "ops/s(avg)" "ns/op" "total_ops" "runs(ops/s)"
        printf "  %s\n" "-------------------------------------------------------------------------------"

        for lang in "${LANGS[@]}"; do
            local bin; bin="$(bin_for "$lang")"
            [ -x "$bin" ] || { warn "skip $lang (no binary)"; continue; }

            for mode in "${MODES[@]}"; do
                supported "$lang" "$mode" "$prim" || continue

                local iters; iters="$(iters_for "$prim" "$mode")"
                local args=(--mode "$mode" --workers "$WORKERS" --tasks "$tasks" --iters "$iters")
                [ "$prim" = "sem" ] && args+=(--permits "$P_PERMITS")

                local ops_sum=0 nspo_sum=0 nspo_avg=0 total_last=0 valid=0 ops_vals=""
                for run in $(seq 1 "$REPEAT"); do
                    local out="$run_dir/sync-${prim}-${lang}-${mode}-r${run}.json"
                    "$bin" "$prim" "${args[@]}" > "$out" 2>/dev/null || true
                    if [ -s "$out" ]; then
                        local ops nspo total reported_mode renamed
                        ops=$(extract_json "$out" ops_per_sec)
                        nspo=$(extract_json "$out" ns_per_op)
                        total=$(extract_json "$out" total_ops)
                        reported_mode=$(grep '"mode"' "$out" 2>/dev/null | sed -E 's/.*"mode"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/' | tail -1)
                        if [ -n "$reported_mode" ] && [ "$reported_mode" != "$mode" ]; then
                            renamed="$run_dir/sync-${prim}-${lang}-${reported_mode}-r${run}.json"
                            mv -f "$out" "$renamed"
                            out="$renamed"
                        fi
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
                    nspo_avg=$(awk -v s="$nspo_sum" -v n="$valid" 'BEGIN { printf "%.2f", s / n }')
                    printf "  %-7s %-7s %16s %12s %14s  [%s]\n" \
                        "$lang" "$mode" "$ops_avg" "$nspo_avg" "$total_last" "$ops_vals"
                else
                    warn "$lang/$mode: no valid output from $REPEAT runs"
                fi
            done
        done
        echo ""
    done

    ok "sync benchmarks complete"
    info "results written to $run_dir"
}

# =============================================================================
# option parsing + dispatch
# =============================================================================
parse_opts() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --prims|-p)   shift; IFS=',' read -ra PRIMS <<< "$1";;
            --langs|-l)   shift; IFS=',' read -ra LANGS <<< "$1";;
            --modes|-m)   shift; IFS=',' read -ra MODES <<< "$1";;
            --workers|-w) shift; WORKERS="$1";;
            --repeat|-r)  shift; REPEAT="$1";;
            --permits)    shift; P_PERMITS="$1";;
            build|bench|all) ;;  # command word, ignore
            *) err "unknown option: $1"; exit 1;;
        esac
        shift
    done
}

usage() {
    cat <<EOF
usage: $0 [build|bench|all] [options]

Commands:
  build   build xylem static lib + C/Go/Rust sync-bench binaries
  bench   run each primitive across languages, write out/results/<ts>/
  all     build + bench   (default)

Options:
  --prims, -p    mutex,cond,waitgroup,sem,channel   primitives to run
  --langs, -l    xylem,go,rust                       languages to compare
  --modes, -m    coro,thread,mixed                   concurrency models
                                                     (go: coro; rust: coro,thread,
                                                      + channel/handoff mixed;
                                                      xylem: all three)
  --workers, -w  0                                   runtime worker threads
                                                     (0 = each runtime default)
  --repeat, -r   3                                   repeat each cell N times
  --permits      4                                   semaphore permits (sem)

Examples:
  $0
  $0 build
  $0 bench --prims mutex --modes coro,thread
  $0 bench --prims mutex,channel --langs xylem,rust --workers 4 --repeat 5
EOF
}

main() {
    local cmd="${1:-all}"; shift || true
    case "$cmd" in
        build) parse_opts "$@"; cmd_build;;
        bench) parse_opts "$@"; cmd_bench;;
        all)   parse_opts "$@"; cmd_build; cmd_bench;;
        -h|--help|help) usage;;
        *) err "unknown command: $cmd"; usage; exit 1;;
    esac
}

main "$@"
