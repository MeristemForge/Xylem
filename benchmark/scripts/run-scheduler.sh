#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Xylem scheduler spawn benchmark (fixed workload: 1,000,000 tasks)
# -----------------------------------------------------------------------------
#   st|mt - build + run the spawn matrix in that mode (xylem vs go vs rust)
#   (no argument) - both ST and MT                           [default]
#
# Fixed matrix -- no options; edit the constants below to change the suite:
#   langs: xylem,go,rust   modes: st+mt   tasks: 1,000,000
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
SCHEDULER_DIR="$BENCH_DIR/scheduler"
SPAWN_DIR="$SCHEDULER_DIR/spawn"
OUT_DIR="$BENCH_DIR/out"
BIN_DIR="$OUT_DIR"
BUILD_DIR="$OUT_DIR/build"
RESULTS_ROOT="$OUT_DIR/results"

# Fixed matrix (no CLI options -- edit these constants to change the suite).
LANGS=(xylem go rust)

info() { printf "[scheduler] %s\n" "$1"; }
ok() { printf "[ok] %s\n" "$1"; }
err() { printf "[err] %s\n" "$1" >&2; }

if [ "$(uname -s)" = "Darwin" ]; then
    PLATFORM="macos"
    ncpu() { sysctl -n hw.ncpu; }
else
    PLATFORM="linux"
    ncpu() { nproc; }
fi

usage() {
    cat <<EOF
usage: $0 [st|mt|help]

Arguments:
  st|mt    build + run the spawn matrix in that mode (xylem vs go vs rust)
  help     this help
  (none)   both ST and MT   [default]

The workload is fixed at 1,000,000 tasks and each cell runs once (no repeat).
Benchmark executables take no args. Edit the constants at the top of this
script to change the matrix.
EOF
}

# =============================================================================
# dependencies (auto-installed at run time when missing)
# =============================================================================

# Fill MISSING_DEPS with anything not installed. Tools are probed with
# command -v so installs via brew, official installers, or rustup are all
# recognized; Linux additionally needs a C toolchain (build-essential).
find_missing_deps() {
    MISSING_DEPS=()
    local tool
    for tool in cmake ninja python3 go; do
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

# Verify one result file against the fixed workload, then print the summary
# line the driver prints per cell. One python invocation does both, so a
# failing check (nonzero exit) still aborts the run under set -e.
verify_result() {
    local result="$1" lang="$2" mode="$3"
    python3 - "$result" "$lang" "$mode" <<'PY'
import json
import math
import os
import sys

path, expected_lang, expected_mode = sys.argv[1:]
with open(path, encoding="utf-8") as result_file:
    result = json.load(result_file)

tasks = 1000000
expected = {
    "benchmark": "spawn",
    "lang": expected_lang,
    "mode": expected_mode,
    "tasks": tasks,
    "completed": tasks,
}
for key, value in expected.items():
    if result.get(key) != value:
        raise SystemExit(f"{key}: expected {value!r}, got {result.get(key)!r}")

workers = result.get("workers")
if not isinstance(workers, int) or isinstance(workers, bool) or workers < 1:
    raise SystemExit("workers must be a positive integer")
expected_workers = 1 if expected_mode == "st" else (os.cpu_count() or 1)
if workers != expected_workers:
    raise SystemExit(f"workers: expected {expected_workers}, got {workers}")

for key in ("elapsed_sec", "tasks_per_sec", "ns_per_task"):
    value = result.get(key)
    if (
        not isinstance(value, (int, float))
        or isinstance(value, bool)
        or not math.isfinite(value)
        or value <= 0
    ):
        raise SystemExit(f"{key} must be positive")

print(f"{result['elapsed_sec']:.6f} {result['tasks_per_sec']:.0f} {result['ns_per_task']:.2f}")
PY
}

build_xylem() {
    info "building xylem static library"
    local cc="${CC:-}"
    if [ -z "$cc" ]; then
        if [ "$(uname -s)" = "Darwin" ]; then
            cc=clang
        elif command -v gcc >/dev/null 2>&1; then
            cc=gcc
        else
            cc=clang
        fi
    fi
    command -v "$cc" >/dev/null 2>&1 || { err "$cc not found"; exit 1; }
    # out/build is shared between the Linux and Windows drivers; a cache
    # generated by the other platform records its own absolute paths and CMake
    # refuses to reuse it. Always start from a fresh build tree.
    rm -rf "$BUILD_DIR"
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$cc" \
        -DXYLEM_ENABLE_TLS=OFF
    cmake --build "$BUILD_DIR" --target xylem -j"$(ncpu)"
    local lib="$BUILD_DIR/libxylem.a"
    [ -f "$lib" ] || { err "xylem library not found: $lib"; exit 1; }
    local cflags=(-std=gnu11 -O3 -DNDEBUG -flto -Wall -Wextra
        -I"$PROJECT_ROOT/include" -I"$PROJECT_ROOT/src")
    "$cc" "${cflags[@]}" "$SPAWN_DIR/xylem/spawn.c" "$lib" -lpthread \
        -flto -s -o "$BIN_DIR/spawn-xylem"
    "$cc" "${cflags[@]}" "$SPAWN_DIR/xylem/spawn-mt.c" "$lib" -lpthread \
        -flto -s -o "$BIN_DIR/spawn-xylem-mt"
}

build_go() {
    command -v go >/dev/null 2>&1 || { err "go not found"; exit 1; }
    info "building go scheduler benchmarks"
    (cd "$SPAWN_DIR/go" && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" \
        -o "$BIN_DIR/spawn-go" ./spawn)
    (cd "$SPAWN_DIR/go" && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" \
        -o "$BIN_DIR/spawn-go-mt" ./spawn-mt)
}

build_rust() {
    command -v cargo >/dev/null 2>&1 || { err "cargo not found"; exit 1; }
    info "building rust scheduler benchmarks"
    (cd "$SPAWN_DIR/rust" && cargo build --release --bins -q \
        --target-dir "$BIN_DIR/cargo")
    cp "$BIN_DIR/cargo/release/spawn-rust" "$BIN_DIR/spawn-rust"
    cp "$BIN_DIR/cargo/release/spawn-rust-mt" "$BIN_DIR/spawn-rust-mt"
}

cmd_build() {
    mkdir -p "$BIN_DIR"
    for lang in "${LANGS[@]}"; do
        case "$lang" in
            xylem) build_xylem ;;
            go) build_go ;;
            rust) build_rust ;;
        esac
    done
    ok "scheduler binaries built"
}

binary_for() {
    case "$1:$2" in
        xylem:st) echo "$BIN_DIR/spawn-xylem" ;;
        xylem:mt) echo "$BIN_DIR/spawn-xylem-mt" ;;
        go:st) echo "$BIN_DIR/spawn-go" ;;
        go:mt) echo "$BIN_DIR/spawn-go-mt" ;;
        rust:st) echo "$BIN_DIR/spawn-rust" ;;
        rust:mt) echo "$BIN_DIR/spawn-rust-mt" ;;
        *) return 1 ;;
    esac
}

cmd_bench() {
    local timestamp run_dir lang mode bin result summary elapsed tasks_per_sec ns_per_task
    timestamp="$(date +%Y%m%d-%H%M%S)"
    run_dir="$RESULTS_ROOT/$timestamp"
    mkdir -p "$run_dir"
    info "results: $run_dir"

    for lang in "${LANGS[@]}"; do
        for mode in "${MODES[@]}"; do
            bin="$(binary_for "$lang" "$mode")"
            [ -x "$bin" ] || { err "missing binary: $bin (run build first)"; exit 1; }
            result="$run_dir/scheduler-spawn-$lang-$mode.json"
            "$bin" > "$result"
            # A failing verify_result (nonzero exit) aborts the run.
            summary="$(verify_result "$result" "$lang" "$mode")"
            read -r elapsed tasks_per_sec ns_per_task <<< "$summary"
            printf "%-6s %-2s  elapsed=%ss  tasks/s=%s  ns/task=%s\n" \
                "$lang" "$mode" "$elapsed" "$tasks_per_sec" "$ns_per_task"
        done
    done
    ok "scheduler benchmarks complete"
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
        ""|st|mt)
            MODES=(st mt)
            [ -n "$target" ] && MODES=("$target")
            ensure_deps
            cmd_build
            cmd_bench
            ;;
        *)
            err "unknown target: $target (must be st|mt|help)"
            usage
            exit 1 ;;
    esac
}

main "$@"
