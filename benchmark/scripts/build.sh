#!/usr/bin/env bash
set -euo pipefail

# Builds all benchmark binaries (Release, stripped).
# Assumes dependencies are installed (run install-deps.sh first).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
OUT_DIR="$BENCH_DIR/bin"

info() { printf "\033[1;34m[build]\033[0m %s\n" "$1"; }
ok()   { printf "\033[1;32m[ok]\033[0m %s\n" "$1"; }

mkdir -p "$OUT_DIR"

CFLAGS_COMMON="-O3 -DNDEBUG -flto -Wall -Wextra"
LDFLAGS_COMMON="-s -flto"
LDFLAGS_SSL="-lssl -lcrypto"

# --- Build xylem library (Release) ------------------------------------------

build_xylem() {
    info "building xylem library..." >&2
    local build_dir="$BENCH_DIR/build"
    cmake -S "$PROJECT_ROOT" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-O3 -DNDEBUG -flto" \
        -DXYLEM_ENABLE_TLS=OFF \
        -G Ninja >/dev/null 2>&1
    ninja -C "$build_dir" xylem -j"$(nproc)" >&2
    echo "$build_dir"
}

XYLEM_BUILD="$(build_xylem)"
XYLEM_INC="$PROJECT_ROOT/include"
XYLEM_LIB="$XYLEM_BUILD/libxylem.a"
ok "xylem built"

# --- TCP servers -------------------------------------------------------------

info "building tcp servers..."

gcc $CFLAGS_COMMON -I"$XYLEM_INC" \
    "$BENCH_DIR/tcp/server/xylem-echo.c" \
    "$XYLEM_LIB" -lpthread $LDFLAGS_SSL $LDFLAGS_COMMON \
    -o "$OUT_DIR/tcp-xylem-echo"

gcc $CFLAGS_COMMON \
    "$BENCH_DIR/tcp/server/libuv-echo.c" \
    -luv $LDFLAGS_COMMON -o "$OUT_DIR/tcp-libuv-echo"

gcc $CFLAGS_COMMON \
    "$BENCH_DIR/tcp/server/libevent-echo.c" \
    -levent $LDFLAGS_COMMON -o "$OUT_DIR/tcp-libevent-echo"

gcc $CFLAGS_COMMON \
    "$BENCH_DIR/tcp/server/libhv-echo.c" \
    -lhv $LDFLAGS_COMMON -o "$OUT_DIR/tcp-libhv-echo"

g++ $CFLAGS_COMMON -std=c++17 \
    "$BENCH_DIR/tcp/server/boost-echo.cpp" \
    -lboost_system -lpthread $LDFLAGS_COMMON -o "$OUT_DIR/tcp-boost-echo"

(cd "$BENCH_DIR/tcp/server/go-echo" && \
    CGO_ENABLED=0 go build -ldflags="-s -w" -o "$OUT_DIR/tcp-go-echo" .)

(cd "$BENCH_DIR/tcp/server/rust-echo" && cargo build --release -q && \
    cp target/release/bench-tcp-rust "$OUT_DIR/tcp-rust-echo" && \
    strip "$OUT_DIR/tcp-rust-echo")

ok "tcp servers"

# --- TCP client --------------------------------------------------------------

info "building tcp client..."
gcc $CFLAGS_COMMON \
    "$BENCH_DIR/tcp/client/tcp-bench.c" \
    $LDFLAGS_COMMON -o "$OUT_DIR/tcp-bench"
ok "tcp client"


# --- UDP servers -------------------------------------------------------------

info "building udp servers..."

gcc $CFLAGS_COMMON -I"$XYLEM_INC" \
    "$BENCH_DIR/udp/server/xylem-echo.c" \
    "$XYLEM_LIB" -lpthread $LDFLAGS_SSL $LDFLAGS_COMMON \
    -o "$OUT_DIR/udp-xylem-echo"

gcc $CFLAGS_COMMON \
    "$BENCH_DIR/udp/server/libuv-echo.c" \
    -luv $LDFLAGS_COMMON -o "$OUT_DIR/udp-libuv-echo"

gcc $CFLAGS_COMMON \
    "$BENCH_DIR/udp/server/libevent-echo.c" \
    -levent $LDFLAGS_COMMON -o "$OUT_DIR/udp-libevent-echo"

gcc $CFLAGS_COMMON \
    "$BENCH_DIR/udp/server/libhv-echo.c" \
    -lhv $LDFLAGS_COMMON -o "$OUT_DIR/udp-libhv-echo"

g++ $CFLAGS_COMMON -std=c++17 \
    "$BENCH_DIR/udp/server/boost-echo.cpp" \
    -lboost_system -lpthread $LDFLAGS_COMMON -o "$OUT_DIR/udp-boost-echo"

(cd "$BENCH_DIR/udp/server/go-echo" && \
    CGO_ENABLED=0 go build -ldflags="-s -w" -o "$OUT_DIR/udp-go-echo" .)

(cd "$BENCH_DIR/udp/server/rust-echo" && cargo build --release -q && \
    cp target/release/bench-udp-rust "$OUT_DIR/udp-rust-echo" && \
    strip "$OUT_DIR/udp-rust-echo")

ok "udp servers"

# --- UDP client --------------------------------------------------------------

info "building udp client..."
gcc $CFLAGS_COMMON \
    "$BENCH_DIR/udp/client/udp-bench-client.c" \
    $LDFLAGS_COMMON -o "$OUT_DIR/udp-bench-client"
ok "udp client"

# --- TLS servers -------------------------------------------------------------

info "building tls servers..."

gcc $CFLAGS_COMMON -I"$XYLEM_INC" \
    "$BENCH_DIR/tls/server/xylem-echo.c" \
    "$XYLEM_LIB" -lpthread $LDFLAGS_SSL $LDFLAGS_COMMON \
    -o "$OUT_DIR/tls-xylem-echo"

gcc $CFLAGS_COMMON \
    "$BENCH_DIR/tls/server/libuv-echo.c" \
    -luv $LDFLAGS_SSL $LDFLAGS_COMMON -o "$OUT_DIR/tls-libuv-echo"

gcc $CFLAGS_COMMON \
    "$BENCH_DIR/tls/server/libevent-echo.c" \
    -levent -levent_openssl $LDFLAGS_SSL $LDFLAGS_COMMON \
    -o "$OUT_DIR/tls-libevent-echo"

gcc $CFLAGS_COMMON \
    "$BENCH_DIR/tls/server/libhv-echo.c" \
    -lhv $LDFLAGS_SSL $LDFLAGS_COMMON -o "$OUT_DIR/tls-libhv-echo"

g++ $CFLAGS_COMMON -std=c++17 \
    "$BENCH_DIR/tls/server/boost-echo.cpp" \
    -lboost_system -lpthread $LDFLAGS_SSL $LDFLAGS_COMMON \
    -o "$OUT_DIR/tls-boost-echo"

(cd "$BENCH_DIR/tls/server/go-echo" && \
    go build -ldflags="-s -w" -o "$OUT_DIR/tls-go-echo" .)

(cd "$BENCH_DIR/tls/server/rust-echo" && cargo build --release -q && \
    cp target/release/bench-tls-rust "$OUT_DIR/tls-rust-echo" && \
    strip "$OUT_DIR/tls-rust-echo")

ok "tls servers"

# --- TLS client --------------------------------------------------------------

info "building tls client..."
gcc $CFLAGS_COMMON \
    "$BENCH_DIR/tls/client/tls-bench-client.c" \
    $LDFLAGS_SSL $LDFLAGS_COMMON -o "$OUT_DIR/tls-bench-client"
ok "tls client"

# --- DTLS servers ------------------------------------------------------------

info "building dtls servers..."

gcc $CFLAGS_COMMON -I"$XYLEM_INC" \
    "$BENCH_DIR/dtls/server/xylem-echo.c" \
    "$XYLEM_LIB" -lpthread $LDFLAGS_SSL $LDFLAGS_COMMON \
    -o "$OUT_DIR/dtls-xylem-echo"

(cd "$BENCH_DIR/dtls/server/go-echo" && \
    go build -ldflags="-s -w" -o "$OUT_DIR/dtls-go-echo" .)

(cd "$BENCH_DIR/dtls/server/rust-echo" && cargo build --release -q && \
    cp target/release/bench-dtls-rust "$OUT_DIR/dtls-rust-echo" && \
    strip "$OUT_DIR/dtls-rust-echo")

ok "dtls servers"

# --- DTLS client -------------------------------------------------------------

info "building dtls client..."
gcc $CFLAGS_COMMON \
    "$BENCH_DIR/dtls/client/dtls-bench-client.c" \
    $LDFLAGS_SSL $LDFLAGS_COMMON -o "$OUT_DIR/dtls-bench-client"
ok "dtls client"

# --- RUDP server -------------------------------------------------------------

info "building rudp server..."

gcc $CFLAGS_COMMON -I"$XYLEM_INC" \
    "$BENCH_DIR/rudp/server/xylem-echo.c" \
    "$XYLEM_LIB" -lpthread $LDFLAGS_SSL $LDFLAGS_COMMON \
    -o "$OUT_DIR/rudp-xylem-echo"

ok "rudp server"

# --- RUDP client -------------------------------------------------------------

info "building rudp client..."
gcc $CFLAGS_COMMON -I"$XYLEM_INC" \
    "$BENCH_DIR/rudp/client/rudp-bench-client.c" \
    "$XYLEM_LIB" -lpthread $LDFLAGS_SSL $LDFLAGS_COMMON \
    -o "$OUT_DIR/rudp-bench-client"
ok "rudp client"

# --- Done --------------------------------------------------------------------

echo ""
ok "all binaries in $OUT_DIR (stripped)"
ls -lh "$OUT_DIR/"
