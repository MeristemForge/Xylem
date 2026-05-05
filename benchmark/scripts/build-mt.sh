#!/usr/bin/env bash
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$PROJECT/benchmark/build"
BIN="$PROJECT/benchmark/bin"

gcc -O3 -DNDEBUG -flto \
    -I"$PROJECT/include" \
    "$PROJECT/benchmark/tcp/server/xylem-echo-mt.c" \
    "$BUILD/libxylem.a" \
    -lpthread -lssl -lcrypto \
    -s -flto \
    -o "$BIN/tcp-xylem-echo-mt"

echo "OK: $BIN/tcp-xylem-echo-mt"
