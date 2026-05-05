#!/usr/bin/env bash
set -euo pipefail

# Build a diagnostic version that counts per-worker coro executions
PROJECT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$PROJECT/benchmark/build"
BIN="$PROJECT/benchmark/bin"

cat > /tmp/diag-echo.c <<'EOF'
#include "xylem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

static _Atomic uint64_t g_resumes = 0;
static _Atomic uint64_t g_polls = 0;
static _Atomic uint64_t g_poll_events = 0;

static void _handle_conn(void* arg) {
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)arg;
    char* buf = (char*)malloc(65536);
    if (!buf) { xylem_tcp_close(conn); return; }
    for (;;) {
        int64_t n = xylem_tcp_recv(conn, buf, 65536);
        if (n <= 0) break;
        if (xylem_tcp_send(conn, buf, (size_t)n) != 0) break;
    }
    free(buf);
    xylem_tcp_close(conn);
}

static void _acceptor(void* arg) {
    int port = *(int*)arg;
    xylem_tcp_opts_t opts = {0};
    opts.disable_mss_clamp = true;
    xylem_tcp_listener_t* server = xylem_tcp_listen("0.0.0.0", (uint16_t)port, &opts);
    if (!server) { xylem_runtime_stop(); return; }
    fprintf(stderr, "listening on 0.0.0.0:%d\n", port);
    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(server);
        if (!conn) break;
        xylem_runtime_spawn(_handle_conn, conn);
    }
}

int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 9000;
    int workers = (argc > 2) ? atoi(argv[2]) : 4;
    xylem_runtime_opts_t rt_opts = {0};
    rt_opts.workers = workers;
    xylem_runtime_start(_acceptor, &port, &rt_opts);
    return 0;
}
EOF

gcc -O3 -DNDEBUG -flto -I"$PROJECT/include" \
    /tmp/diag-echo.c "$BUILD/libxylem.a" \
    -lpthread -lssl -lcrypto -s -flto \
    -o "$BIN/tcp-xylem-diag"

pkill -f "tcp-.*-echo" 2>/dev/null || true
sleep 1

# Run with 1 worker
echo "=== 1 worker ==="
"$BIN/tcp-xylem-diag" 9000 1 >/dev/null 2>&1 &
PID=$!; sleep 2
"$BIN/tcp-bench" throughput -n 1000 -d 5 -p 9000 2>/dev/null
kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
sleep 1

# Run with 4 workers and trace syscalls per thread
echo ""
echo "=== 4 workers (strace per-thread syscall count, 3s) ==="
strace -c -f -e trace=epoll_wait,epoll_ctl,recvfrom,sendto \
    "$BIN/tcp-xylem-diag" 9000 4 2>/tmp/diag-strace.txt &
PID=$!; sleep 3
"$BIN/tcp-bench" throughput -n 1000 -d 3 -p 9000 2>/dev/null
sleep 1
kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
echo ""
cat /tmp/diag-strace.txt | grep -E "epoll_wait|recvfrom|sendto|total" | tail -10
