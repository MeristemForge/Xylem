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
 * TCP Echo Example (coroutine)
 *
 * Demonstrates coroutine-based TCP networking: one coroutine runs the
 * echo server, another runs the client. Both execute concurrently within
 * a single process / single runtime.
 *
 * Usage: tcp-echo
 */

#include "xylem.h"

#define PORT 9000

static void _server(void* arg) {
    (void)arg;

    xylem_tcp_listener_t* ln = xylem_tcp_listen("127.0.0.1", PORT, NULL);
    if (!ln) {
        xylem_loge("[server] failed to listen on port %d", PORT);
        return;
    }
    xylem_logi("[server] listening on 127.0.0.1:%d", PORT);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    if (conn) {
        char    buf[1024];
        int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
        if (n > 0) {
            xylem_logi("[server] recv: %.*s", (int)n, buf);
            xylem_tcp_send(conn, buf, (size_t)n);
        }
        xylem_tcp_close(conn);
    }

    xylem_tcp_close_listener(ln);
}

static void _client(void* arg) {
    (void)arg;

    xylem_tcp_conn_t* conn = xylem_tcp_dial("127.0.0.1", PORT, 5000, NULL);
    if (!conn) {
        xylem_loge("[client] failed to connect");
        return;
    }
    xylem_logi("[client] connected");

    xylem_tcp_send(conn, "hello", 5);

    char    buf[256];
    int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    if (n > 0) {
        xylem_logi("[client] echo: %.*s", (int)n, buf);
    }

    xylem_tcp_close(conn);
    xylem_logi("[client] done");
}

static void _main(void* arg) {
    (void)arg;
    xylem_runtime_spawn(_server, NULL);
    xylem_runtime_sleep(100);
    xylem_runtime_spawn(_client, NULL);
}

int main(void) {
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);
    xylem_runtime_run(_main, NULL, NULL);
    xylem_logger_deinit();
    return 0;
}
