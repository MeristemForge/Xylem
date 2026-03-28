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

#include "assert.h"
#include "xylem/ws/xylem-ws-common.h"
#include "xylem/ws/xylem-ws-client.h"
#include "xylem/ws/xylem-ws-server.h"
#include "xylem/xylem-loop.h"

static void test_dial_null_loop(void) {
    xylem_ws_handler_t handler = {0};
    ASSERT(xylem_ws_dial(NULL, "ws://localhost", &handler, NULL) == NULL);
}

static void test_dial_null_url(void) {
    xylem_loop_t* loop = xylem_loop_create();
    xylem_ws_handler_t handler = {0};
    ASSERT(xylem_ws_dial(loop, NULL, &handler, NULL) == NULL);
    xylem_loop_destroy(loop);
}

static void test_dial_null_handler(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(xylem_ws_dial(loop, "ws://localhost", NULL, NULL) == NULL);
    xylem_loop_destroy(loop);
}

static void test_send_null(void) {
    ASSERT(xylem_ws_send(NULL, XYLEM_WS_OPCODE_TEXT, "hi", 2) == -1);
}

static void test_send_binary_null(void) {
    ASSERT(xylem_ws_send(NULL, XYLEM_WS_OPCODE_BINARY, "data", 4) == -1);
}

static void test_ping_null(void) {
    ASSERT(xylem_ws_ping(NULL, "p", 1) == -1);
}

static void test_ping_null_no_payload(void) {
    ASSERT(xylem_ws_ping(NULL, NULL, 0) == -1);
}

static void test_close_null(void) {
    ASSERT(xylem_ws_close(NULL, 1000, NULL, 0) == -1);
}

static void test_close_null_with_reason(void) {
    ASSERT(xylem_ws_close(NULL, 1001, "going away", 10) == -1);
}

static void test_userdata_null(void) {
    ASSERT(xylem_ws_get_userdata(NULL) == NULL);
    xylem_ws_set_userdata(NULL, NULL);
}

static void test_listen_null_loop(void) {
    xylem_ws_srv_cfg_t cfg = {0};
    ASSERT(xylem_ws_listen(NULL, &cfg) == NULL);
}

static void test_listen_null_cfg(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(xylem_ws_listen(loop, NULL) == NULL);
    xylem_loop_destroy(loop);
}

static void test_listen_create_destroy(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_ws_handler_t handler = {0};
    xylem_ws_srv_cfg_t cfg = {0};
    cfg.host    = "127.0.0.1";
    cfg.port    = 0;
    cfg.handler = &handler;

    xylem_ws_server_t* srv = xylem_ws_listen(loop, &cfg);
    ASSERT(srv != NULL);
    xylem_ws_close_server(srv);

    xylem_loop_destroy(loop);
}

static void test_close_server_null(void) {
    xylem_ws_close_server(NULL);
}

int main(void) {
    /* Client dial */
    test_dial_null_loop();
    test_dial_null_url();
    test_dial_null_handler();

    /* Send */
    test_send_null();
    test_send_binary_null();

    /* Ping */
    test_ping_null();
    test_ping_null_no_payload();

    /* Close */
    test_close_null();
    test_close_null_with_reason();

    /* Userdata */
    test_userdata_null();

    /* Server */
    test_listen_null_loop();
    test_listen_null_cfg();
    test_listen_create_destroy();
    test_close_server_null();

    return 0;
}
