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
 * Xylem UDP Echo Benchmark Server (single-threaded)
 *
 * A single coroutine binds the socket then loops xylem_udp_recv() /
 * xylem_udp_send(), echoing each datagram back to its sender. recv/send
 * suspend the coroutine until the socket is ready. Runs under a single
 * scheduler worker (ST); the public UDP API exposes no SO_REUSEPORT, so
 * there is no MT variant.
 *
 * Usage: udp-xylem-echo [port]
 */

#include "xylem.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT  9001
#define READ_BUF_SIZE 65536
#define HOST_BUF_SIZE 46

/* Parse a base-10 integer in [min, max]; returns fallback on any error. */
static long _parse_int(const char* s, long min, long max, long fallback) {
    char* end = NULL;
    long  v   = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < min || v > max) {
        return fallback;
    }
    return v;
}

static void _server(void* arg) {
    uint16_t port = *(uint16_t*)arg;

    xylem_udp_chan_t* udp = xylem_udp_listen("0.0.0.0", port);
    if (!udp) {
        fprintf(stderr, "udp echo: bind failed port=%" PRIu16 "\n", port);
        xylem_shutdown();
        return;
    }

    fprintf(
        stderr,
        "xylem udp echo server listening on 0.0.0.0:%" PRIu16 "\n",
        port);

    char* buf = (char*)malloc(READ_BUF_SIZE);
    if (!buf) {
        xylem_udp_close(udp);
        xylem_shutdown();
        return;
    }

    for (;;) {
        char     host[HOST_BUF_SIZE];
        uint16_t peer_port = 0;
        int      n         = xylem_udp_recv(
            udp,
            buf,
            READ_BUF_SIZE,
            host,
            sizeof(host),
            &peer_port);
        if (n < 0) {
            break;
        }
        xylem_udp_send(udp, buf, n, host, peer_port);
    }

    free(buf);
    xylem_udp_close(udp);
}

int main(int argc, char** argv) {
    uint16_t port = DEFAULT_PORT;
    if (argc > 1) {
        port = (uint16_t)_parse_int(argv[1], 1, 65535, DEFAULT_PORT);
    }

    xylem_opts_t rt_opts = {.workers = 1};
    xylem_run(_server, &port, &rt_opts);
    return 0;
}
