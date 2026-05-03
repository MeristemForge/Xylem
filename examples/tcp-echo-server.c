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
 * TCP Echo Server (coroutine)
 *
 * Listens on 127.0.0.1:9000 and echoes back every line it receives.
 *
 * Usage: tcp-echo-server
 * Test:  tcp-echo-client  (or: echo "hello" | nc 127.0.0.1 9000)
 */

#include "xylem.h"

#define LISTEN_PORT 9000

static void _handle_conn(void* arg) {
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)arg;
    xylem_logi("client connected");

    char line[1024];
    for (;;) {
        ssize_t n = xylem_tcp_recv_line(conn, line, sizeof(line));
        if (n < 0) break;

        xylem_logi("recv: %.*s", (int)n, line);

        line[n]     = '\r';
        line[n + 1] = '\n';
        if (xylem_tcp_send(conn, line, (size_t)n + 2) != 0)
            break;
    }

    xylem_logi("client disconnected");
    xylem_tcp_close(conn);
}

static void _server_main(void* arg) {
    (void)arg;
    xylem_tcp_listener_t* server = xylem_tcp_listen("127.0.0.1",
                                                   LISTEN_PORT, NULL);
    if (!server) {
        xylem_loge("failed to listen on port %d", LISTEN_PORT);
        xylem_runtime_stop();
        return;
    }

    xylem_logi("tcp echo server listening on 127.0.0.1:%d", LISTEN_PORT);

    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(server);
        if (!conn) break;
        xylem_runtime_spawn(_handle_conn, conn);
    }
}

int main(void) {
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);
    xylem_runtime_start(_server_main, NULL, NULL);
    xylem_logger_deinit();
    return 0;
}
