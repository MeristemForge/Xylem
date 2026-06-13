/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

/**
 * Xylem TCP Echo Benchmark Server (single-threaded)
 *
 * One acceptor coroutine loops xylem_tcp_accept() and spawns a handler
 * coroutine per connection that echoes received bytes back. Runs under a
 * single scheduler worker (ST).
 *
 * Usage: tcp-xylem-echo [port]
 */

#include "xylem.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT  9000
#define READ_BUF_SIZE 65536

/* Parse a base-10 integer in [min, max]; returns fallback on any error. */
static long _parse_int(const char* s, long min, long max, long fallback) {
    char* end = NULL;
    long  v   = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < min || v > max) {
        return fallback;
    }
    return v;
}

static void _handle_conn(void* arg) {
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)arg;

    /* Heap buffer: 64 KiB would consume half the 128 KiB coroutine stack. */
    char* buf = (char*)malloc(READ_BUF_SIZE);
    if (!buf) {
        xylem_tcp_close(conn);
        return;
    }

    for (;;) {
        int n = xylem_tcp_read(conn, buf, READ_BUF_SIZE);
        if (n <= 0) {
            break;
        }
        if (xylem_tcp_write(conn, buf, n) != 0) {
            break;
        }
    }

    free(buf);
    xylem_tcp_close(conn);
}

static void _acceptor(void* arg) {
    uint16_t port = *(uint16_t*)arg;

    xylem_tcp_opts_t opts = {.disable_mss_clamp = true};

    xylem_tcp_listener_t* server = xylem_tcp_listen("0.0.0.0", port, &opts);
    if (!server) {
        fprintf(stderr, "tcp echo: listen failed port=%" PRIu16 "\n", port);
        xylem_shutdown();
        return;
    }

    fprintf(
        stderr,
        "xylem tcp echo server listening on 0.0.0.0:%" PRIu16 "\n",
        port);

    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(server);
        if (!conn) {
            break;
        }
        xylem_spawn(_handle_conn, conn);
    }

    xylem_tcp_close_listener(server);
}

int main(int argc, char** argv) {
    uint16_t port = DEFAULT_PORT;
    if (argc > 1) {
        port = (uint16_t)_parse_int(argv[1], 1, 65535, DEFAULT_PORT);
    }

    xylem_opts_t rt_opts = {.workers = 1};
    xylem_run(_acceptor, &port, &rt_opts);
    return 0;
}
