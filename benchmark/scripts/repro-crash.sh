#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

pkill -f "tcp-.*-echo" 2>/dev/null || true
sleep 1

cmake -S . -B benchmark/build-debug \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-g -O0 -fsanitize=address -fno-omit-frame-pointer" \
    -DXYLEM_ENABLE_TLS=OFF -G Ninja 2>&1 | tail -2

ninja -C benchmark/build-debug xylem -j4 2>&1 | tail -3

gcc -g -O0 -fsanitize=address -fno-omit-frame-pointer \
    -Iinclude \
    -x c /dev/stdin \
    benchmark/build-debug/libxylem.a \
    -lpthread -lssl -lcrypto \
    -o benchmark/bin/tcp-xylem-echo-mt-debug <<'EOF'
#include "xylem.h"
#include <stdio.h>
#include <stdlib.h>
static void _handle_conn(void* arg) {
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)arg;
    char buf[4096];
    for (;;) {
        int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
        if (n <= 0) break;
        if (xylem_tcp_send(conn, buf, (size_t)n) != 0) break;
    }
    xylem_tcp_close(conn);
}
static void _acceptor(void* arg) {
    int port = *(int*)arg;
    xylem_tcp_opts_t opts = {0};
    opts.disable_mss_clamp = true;
    xylem_tcp_listener_t* server = xylem_tcp_listen("0.0.0.0", (uint16_t)port, &opts);
    if (!server) { xylem_runtime_stop(); return; }
    fprintf(stderr, "listening on :%d w=4\n", port);
    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(server);
        if (!conn) break;
        xylem_runtime_spawn(_handle_conn, conn);
    }
}
int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 9000;
    xylem_runtime_opts_t rt = {0};
    rt.workers = 4;
    xylem_runtime_start(_acceptor, &port, &rt);
    return 0;
}
EOF

echo "=== Running with ASAN ==="
benchmark/bin/tcp-xylem-echo-mt-debug 9000 2>/tmp/asan.log &
PID=$!
sleep 3
benchmark/bin/tcp-bench connrate -c 256 -d 3 -p 9000 || true
sleep 2
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
echo "=== ASAN output (last 60 lines) ==="
tail -60 /tmp/asan.log
