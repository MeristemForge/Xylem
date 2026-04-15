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
 * TCP Echo Client
 *
 * Connects to 127.0.0.1:9000, sends "hello", prints the echoed
 * response, then disconnects.
 *
 * Usage: tcp-echo-client
 * Pair:  tcp-echo-server
 */

#include "xylem.h"
#include "xylem/xylem-tcp.h"

#include <string.h>

#define SERVER_PORT 9000

static xylem_loop_t* _loop;

static void _on_connect(xylem_tcp_conn_t* conn) {
    xylem_logi("connected to server");
    xylem_tcp_send(conn, "hello\r\n", 7);
}

static void _on_read(xylem_tcp_conn_t* conn, void* data, size_t len) {
    xylem_logi("echo: %.*s", (int)len, (char*)data);
    xylem_tcp_close(conn);
}

static void _on_close(xylem_tcp_conn_t* conn, int err, const char* errmsg) {
    (void)conn;
    (void)err;
    xylem_logi("disconnected (%s)", errmsg);
    xylem_loop_stop(_loop);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    _loop = xylem_loop_create();

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", SERVER_PORT, &addr);

    xylem_tcp_handler_t handler = {
        .on_connect = _on_connect,
        .on_read    = _on_read,
        .on_close   = _on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type          = XYLEM_TCP_FRAME_DELIM;
    opts.framing.delim.delim     = "\r\n";
    opts.framing.delim.delim_len = 2;

    xylem_tcp_conn_t* conn = xylem_tcp_dial(_loop, &addr,
                                            &handler, &opts);
    if (!conn) {
        xylem_loge("failed to connect");
        return 1;
    }

    xylem_loop_run(_loop);

    xylem_loop_destroy(_loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
