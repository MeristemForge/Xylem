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
 * TCP Echo Client (coroutine)
 *
 * Connects to 127.0.0.1:9000, sends "hello", prints the echoed
 * response, then disconnects.
 *
 * Usage: tcp-echo-client
 * Pair:  tcp-echo-server
 */

#include "xylem.h"

#include <string.h>

#define SERVER_PORT 9000

static void _client_main(void* arg) {
    (void)arg;

    xylem_tcp_conn_t* conn = xylem_tcp_dial("127.0.0.1", SERVER_PORT, NULL);
    if (!conn) {
        xylem_loge("failed to connect");
        xylem_runtime_stop();
        return;
    }

    xylem_logi("connected to server");
    xylem_tcp_send(conn, "hello\r\n", 7);

    char line[256];
    ssize_t n = xylem_tcp_recv_line(conn, line, sizeof(line));
    if (n > 0) {
        xylem_logi("echo: %.*s", (int)n, line);
    }

    xylem_tcp_close(conn);
    xylem_logi("disconnected");
    xylem_runtime_stop();
}

int main(void) {
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);
    xylem_runtime_start(_client_main, NULL, NULL);
    xylem_logger_deinit();
    return 0;
}
