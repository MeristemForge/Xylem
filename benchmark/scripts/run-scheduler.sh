#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
SCHEDULER_DIR="$BENCH_DIR/scheduler"
SPAWN_DIR="$SCHEDULER_DIR/spawn"
OUT_DIR="$BENCH_DIR/out"
BIN_DIR="$OUT_DIR"
BUILD_DIR="$OUT_DIR/build"
RESULTS_ROOT="$OUT_DIR/results"

LANGS_TEXT="${LANGS:-xylem,go,rust}"
REPEAT="${REPEAT:-3}"

info() { printf "[scheduler] %s\n" "$1"; }
ok() { printf "[ok] %s\n" "$1"; }
err() { printf "[err] %s\n" "$1" >&2; }

ncpu() {
    if [ "$(uname -s)" = "Darwin" ]; then
        sysctl -n hw.ncpu
    else
        nproc
    fi
}

usage() {
    cat <<EOF
usage: $0 [build|bench|all] [options...]

Commands:
  build             build the six scheduler benchmark binaries
  bench             run both ST and MT modes and write results/<timestamp>/
  all               build + bench (default)

Options:
  --langs, -l LIST  comma-separated subset of xylem,go,rust
  --repeat, -r N    runs per language/mode (default: 3)

The workload is fixed at 1,000,000 tasks. Benchmark executables take no args.
EOF
}

parse_opts() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --langs|-l)
                [ $# -ge 2 ] || { err "--langs requires a value"; exit 1; }
                LANGS_TEXT="$2"
                shift 2
                ;;
            --repeat|-r)
                [ $# -ge 2 ] || { err "--repeat requires a value"; exit 1; }
                REPEAT="$2"
                shift 2
                ;;
            *)
                err "unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done
    case "$REPEAT" in
        ''|*[!0-9]*) err "repeat must be a positive integer"; exit 1 ;;
    esac
    [ "$REPEAT" -gt 0 ] || { err "repeat must be a positive integer"; exit 1; }
    IFS=',' read -r -a LANGS <<< "$LANGS_TEXT"
    for lang in "${LANGS[@]}"; do
        case "$lang" in
            xylem|go|rust) ;;
            *) err "unsupported language: $lang"; exit 1 ;;
        esac
    done
}

require_tools() {
    for tool in cmake ninja python3; do
        command -v "$tool" >/dev/null 2>&1 || { err "$tool not found"; exit 1; }
    done
}

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
PY
}

summarize_results() {
    local results_dir="$1" lang="$2" mode="$3" repeat="$4"
    python3 - "$results_dir" "$lang" "$mode" "$repeat" <<'PY'
import json
import sys
from pathlib import Path

results_dir = Path(sys.argv[1])
lang, mode, repeat = sys.argv[2], sys.argv[3], int(sys.argv[4])
results = []
for run in range(1, repeat + 1):
    path = results_dir / f"scheduler-spawn-{lang}-{mode}-r{run}.json"
    with path.open(encoding="utf-8") as result_file:
        results.append(json.load(result_file))

count = len(results)
values = [
    sum(result[key] for result in results) / count
    for key in ("elapsed_sec", "tasks_per_sec", "ns_per_task")
]
print(f"{values[0]:.6f} {values[1]:.0f} {values[2]:.2f}")
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
    require_tools
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
    require_tools
    local timestamp run_dir lang mode run bin result elapsed tasks_per_sec ns_per_task
    timestamp="$(date +%Y%m%d-%H%M%S)"
    run_dir="$RESULTS_ROOT/$timestamp"
    mkdir -p "$run_dir"
    info "results: $run_dir (repeat=$REPEAT)"

    for lang in "${LANGS[@]}"; do
        for mode in st mt; do
            bin="$(binary_for "$lang" "$mode")"
            [ -x "$bin" ] || { err "missing binary: $bin (run build first)"; exit 1; }
            for run in $(seq 1 "$REPEAT"); do
                result="$run_dir/scheduler-spawn-$lang-$mode-r$run.json"
                "$bin" > "$result"
                verify_result "$result" "$lang" "$mode"
            done
            read -r elapsed tasks_per_sec ns_per_task < <(
                summarize_results "$run_dir" "$lang" "$mode" "$REPEAT"
            )
            printf "%-6s %-2s  elapsed=%ss  tasks/s=%s  ns/task=%s\n" \
                "$lang" "$mode" "$elapsed" "$tasks_per_sec" "$ns_per_task"
        done
    done
    ok "scheduler benchmarks complete"
}

main() {
    local command="${1:-all}"
    shift || true
    case "$command" in
        -h|--help|help) usage ;;
        build|bench|all)
            parse_opts "$@"
            case "$command" in
                build) cmd_build ;;
                bench) cmd_bench ;;
                all) cmd_build; cmd_bench ;;
            esac
            ;;
        *) err "unknown command: $command"; usage; exit 1 ;;
    esac
}

main "$@"
