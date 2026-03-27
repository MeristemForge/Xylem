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
 * WebSocket Echo Server
 *
 * Listens on 127.0.0.1:9002 and echoes back every WebSocket message.
 *
 * Usage: ws-echo-server
 * Test:  ws-echo-client  (or any WebSocket client connecting to ws://127.0.0.1:9002)
 */

#include "xylem.h"
#include "xylem/ws/xylem-ws-server.h"

#define LISTEN_PORT 9002

static void _on_accept(xylem_ws_conn_t* conn) {
    (void)conn;
    xylem_logi("client connected");
}

static void _on_message(xylem_ws_conn_t* conn,
                        xylem_ws_opcode_t opcode,
                        const void* data, size_t len) {
    xylem_logi("recv %zu bytes: %.*s", len, (int)len, (const char*)data);
    xylem_ws_send(conn, opcode, data, len);
}

static void _on_close(xylem_ws_conn_t* conn,
                      uint16_t code, const char* reason, size_t reason_len) {
    (void)conn;
    (void)reason;
    (void)reason_len;
    xylem_logi("client disconnected (code=%u)", code);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_loop_t* loop = xylem_loop_create();

    xylem_ws_handler_t handler = {
        .on_accept  = _on_accept,
        .on_message = _on_message,
        .on_close   = _on_close,
    };

    xylem_ws_srv_cfg_t cfg = {
        .host    = "127.0.0.1",
        .port    = LISTEN_PORT,
        .handler = &handler,
    };

    xylem_ws_server_t* server = xylem_ws_listen(loop, &cfg);
    if (!server) {
        xylem_loge("failed to listen on port %d", LISTEN_PORT);
        return 1;
    }

    xylem_logi("ws echo server listening on 127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
