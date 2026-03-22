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
 * TCP Echo Server
 *
 * Listens on 127.0.0.1:9000 and echoes back every message it receives.
 * Uses delimiter-based framing with "\r\n" so each line is one message.
 *
 * Usage: tcp-echo-server
 * Test:  tcp-echo-client  (or: echo "hello" | nc 127.0.0.1 9000)
 */

#include "xylem/xylem-tcp.h"
#include <string.h>

#define LISTEN_PORT 9000

static void _on_accept(xylem_tcp_conn_t* conn) {
    (void)conn;
    xylem_logi("client connected");
}

static void _on_read(xylem_tcp_conn_t* conn, void* data, size_t len) {
    xylem_logi("recv %zu bytes: %.*s", len, (int)len, (char*)data);

    /* Echo back with delimiter. */
    char buf[1024];
    if (len + 2 <= sizeof(buf)) {
        memcpy(buf, data, len);
        buf[len]     = '\r';
        buf[len + 1] = '\n';
        xylem_tcp_send(conn, buf, len + 2);
    }
}

static void _on_close(xylem_tcp_conn_t* conn, int err) {
    (void)conn;
    xylem_logi("client disconnected (err=%d)", err);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_loop_t loop;
    xylem_loop_init(&loop);

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", LISTEN_PORT, &addr);

    xylem_tcp_handler_t handler = {
        .on_accept = _on_accept,
        .on_read   = _on_read,
        .on_close  = _on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type          = XYLEM_TCP_FRAME_DELIM;
    opts.framing.delim.delim     = "\r\n";
    opts.framing.delim.delim_len = 2;

    xylem_tcp_server_t* server = xylem_tcp_listen(&loop, &addr,
                                                  &handler, &opts);
    if (!server) {
        xylem_loge("failed to listen on port %d", LISTEN_PORT);
        return 1;
    }

    xylem_logi("tcp echo server listening on 127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(&loop);

    xylem_loop_deinit(&loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
