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
 * WebSocket Secure Echo Client
 *
 * Connects to wss://127.0.0.1:9003/echo, sends "hello", prints the
 * echoed response, then closes with code 1000.
 *
 * Usage: wss-echo-client
 * Pair:  wss-echo-server cert.pem key.pem
 */

#include "xylem.h"
#include "xylem/ws/xylem-ws-client.h"

static xylem_loop_t* _loop;

static void _on_open(xylem_ws_conn_t* conn) {
    xylem_logi("tls handshake complete");
    xylem_ws_send(conn, XYLEM_WS_OPCODE_TEXT, "hello", 5);
}

static void _on_message(xylem_ws_conn_t* conn,
                        xylem_ws_opcode_t opcode,
                        const void* data, size_t len) {
    (void)opcode;
    xylem_logi("echo: %.*s", (int)len, (const char*)data);
    xylem_ws_close(conn, 1000, NULL, 0);
}

static void _on_close(xylem_ws_conn_t* conn,
                      uint16_t code, const char* reason, size_t reason_len) {
    (void)conn;
    (void)reason;
    (void)reason_len;
    xylem_logi("disconnected (code=%u)", code);
    xylem_loop_stop(_loop);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    _loop = xylem_loop_create();

    xylem_ws_handler_t handler = {
        .on_open    = _on_open,
        .on_message = _on_message,
        .on_close   = _on_close,
    };

    xylem_ws_conn_t* conn = xylem_ws_dial(_loop,
                                          "wss://127.0.0.1:9003/echo",
                                          &handler, NULL);
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
