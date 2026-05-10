#!/usr/bin/env bash
# Build a stats-instrumented xylem echo server into bin/tcp-xylem-echo-mt-stats.
# Compiles libxylem.a with -DXYLEM_IOWAIT_STATS into build-stats/ so the
# normal Release artifacts stay untouched.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")"/../.. && pwd)"
BENCH="$ROOT/benchmark"
BUILD="$BENCH/build-stats"
BIN="$BENCH/bin"

mkdir -p "$BUILD" "$BIN"

if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS='-O3 -DNDEBUG -DXYLEM_IOWAIT_STATS -flto' \
        -DXYLEM_ENABLE_TLS=OFF \
        -G Ninja >/dev/null
fi

ninja -C "$BUILD" xylem >/dev/null
LIB="$BUILD/libxylem.a"

gcc -O3 -DNDEBUG -DXYLEM_IOWAIT_STATS -flto -Wall -Wextra \
    -I"$ROOT/include" -I"$ROOT/src" \
    "$BENCH/tcp/server/xylem-echo-mt-stats.c" \
    "$LIB" -lpthread -s -flto \
    -o "$BIN/tcp-xylem-echo-mt-stats"

echo "built $BIN/tcp-xylem-echo-mt-stats"
