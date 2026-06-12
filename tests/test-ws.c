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

#include "xylem.h"
#include "xylem/net/xylem-ws.h"
#include "assert.h"

#include <stdio.h>
#include <string.h>

static void _srv_echo_handler(xylem_ws_conn_t* ws, void* ud) {
    xylem_ws_msg_t msg;
    while (xylem_ws_recv(ws, &msg) == 0) {
        xylem_ws_send(ws, msg.opcode, msg.data, msg.len);
        xylem_ws_msg_free(&msg);
    }
    xylem_ws_close(ws, 1000, NULL, 0);
    xylem_waitgroup_done((xylem_waitgroup_t*)ud);
}

static xylem_ws_listener_t* _listen(xylem_ws_handler_fn_t handler,
                                    const xylem_ws_opts_t* opts,
                                    xylem_waitgroup_t* wg) {
    xylem_waitgroup_add(wg, 1);
    xylem_ws_listener_t* l =
        xylem_ws_listen("127.0.0.1", 0, handler, wg, opts);
    ASSERT(l != NULL);
    return l;
}

static void _drain(xylem_ws_conn_t* c, xylem_ws_listener_t* l,
                   xylem_waitgroup_t* wg) {
    xylem_ws_close(c, 1000, NULL, 0);
    xylem_waitgroup_wait(wg);
    xylem_ws_close_listener(l);
    xylem_waitgroup_destroy(wg);
}

static xylem_ws_conn_t* _connect(xylem_ws_listener_t* l,
                                 const xylem_ws_opts_t* cli_opts) {
    uint16_t port = xylem_ws_listener_port(l);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);

    xylem_ws_conn_t* c = xylem_ws_dial(url, cli_opts);
    ASSERT(c != NULL);
    return c;
}

static void _echo_roundtrip(xylem_ws_conn_t* c, xylem_ws_opcode_t opcode,
                            const void* data, size_t len) {
    ASSERT(xylem_ws_send(c, opcode, data, len) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == opcode);
    ASSERT(msg.len == len);
    ASSERT(memcmp(msg.data, data, len) == 0);
    xylem_ws_msg_free(&msg);
}

static void test_text_echo(void* arg) {
    (void)arg;
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_handler, NULL, wg);

    xylem_ws_conn_t* c = _connect(l, NULL);
    const char*      text = "hello websocket";
    _echo_roundtrip(c, XYLEM_WS_TEXT, text, strlen(text));

    _drain(c, l, wg);
    xylem_shutdown();
}

static void test_binary_echo(void* arg) {
    (void)arg;
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_handler, NULL, wg);

    xylem_ws_conn_t* c    = _connect(l, NULL);
    uint8_t          data[] = {0x00, 0x01, 0x02, 0xFF, 0xFE};
    _echo_roundtrip(c, XYLEM_WS_BINARY, data, sizeof(data));

    _drain(c, l, wg);
    xylem_shutdown();
}

static void test_multiple_messages(void* arg) {
    (void)arg;
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_handler, NULL, wg);

    xylem_ws_conn_t* c = _connect(l, NULL);
    for (int i = 0; i < 10; i++) {
        char buf[32];
        int  len = snprintf(buf, sizeof(buf), "msg-%d", i);
        _echo_roundtrip(c, XYLEM_WS_TEXT, buf, (size_t)len);
    }

    _drain(c, l, wg);
    xylem_shutdown();
}

static void test_large_message(void* arg) {
    (void)arg;
    xylem_ws_opts_t      opts = {.fragment_threshold = 1024};
    xylem_waitgroup_t*   wg   = xylem_waitgroup_create();
    xylem_ws_listener_t* l    = _listen(_srv_echo_handler, &opts, wg);

    xylem_ws_conn_t* c = _connect(l, &opts);

    size_t   big_len = 8192;
    uint8_t* big     = (uint8_t*)malloc(big_len);
    ASSERT(big != NULL);
    for (size_t i = 0; i < big_len; i++) {
        big[i] = (uint8_t)(i & 0xFF);
    }

    _echo_roundtrip(c, XYLEM_WS_BINARY, big, big_len);

    free(big);
    _drain(c, l, wg);
    xylem_shutdown();
}

static void _srv_close_handler(xylem_ws_conn_t* ws, void* ud) {
    xylem_ws_msg_t msg;
    if (xylem_ws_recv(ws, &msg) == 0) {
        xylem_ws_msg_free(&msg);
    }
    xylem_ws_close(ws, 1000, "bye", 3);
    xylem_waitgroup_done((xylem_waitgroup_t*)ud);
}

static void test_server_close(void* arg) {
    (void)arg;
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_close_handler, NULL, wg);

    xylem_ws_conn_t* c = _connect(l, NULL);
    ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, "trigger", 7) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == -1);
    ASSERT(xylem_ws_close_code(c) == 1000);

    _drain(c, l, wg);
    xylem_shutdown();
}

static void test_null_guards(void* arg) {
    (void)arg;
    ASSERT(xylem_ws_dial(NULL, NULL) == NULL);
    ASSERT(xylem_ws_dial("http://bad", NULL) == NULL);
    ASSERT(xylem_ws_send(NULL, XYLEM_WS_TEXT, "x", 1) == -1);
    ASSERT(xylem_ws_recv(NULL, NULL) == -1);
    ASSERT(xylem_ws_ping(NULL, NULL, 0) == -1);
    ASSERT(xylem_ws_close(NULL, 1000, NULL, 0) == -1);
    ASSERT(xylem_ws_close_code(NULL) == 0);
    ASSERT(xylem_ws_get_userdata(NULL) == NULL);
    xylem_ws_set_userdata(NULL, NULL);
    xylem_ws_msg_free(NULL);
    xylem_ws_close_listener(NULL);
    ASSERT(xylem_ws_listener_port(NULL) == 0);
    xylem_shutdown();
}

static void test_deflate_text_echo(void* arg) {
    (void)arg;
    xylem_ws_opts_t      opts = {.permessage_deflate = true};
    xylem_waitgroup_t*   wg   = xylem_waitgroup_create();
    xylem_ws_listener_t* l    = _listen(_srv_echo_handler, &opts, wg);

    xylem_ws_conn_t* c = _connect(l, &opts);
    const char* text = "hello permessage-deflate compression test!";
    _echo_roundtrip(c, XYLEM_WS_TEXT, text, strlen(text));

    _drain(c, l, wg);
    xylem_shutdown();
}

static void test_deflate_context_takeover(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts = {
        .permessage_deflate       = true,
        .deflate_context_takeover = true,
    };
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_handler, &opts, wg);

    xylem_ws_conn_t* c = _connect(l, &opts);
    for (int i = 0; i < 10; i++) {
        char buf[128];
        int  len = snprintf(
            buf, sizeof(buf),
            "message number %d with repeated content for compression", i);
        _echo_roundtrip(c, XYLEM_WS_TEXT, buf, (size_t)len);
    }

    _drain(c, l, wg);
    xylem_shutdown();
}

static void test_deflate_large_binary(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts = {
        .permessage_deflate = true,
        .fragment_threshold = 4096,
    };
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_handler, &opts, wg);

    xylem_ws_conn_t* c = _connect(l, &opts);

    size_t   big_len = 32768;
    uint8_t* big     = (uint8_t*)malloc(big_len);
    ASSERT(big != NULL);
    memset(big, 'A', big_len);

    _echo_roundtrip(c, XYLEM_WS_BINARY, big, big_len);

    free(big);
    _drain(c, l, wg);
    xylem_shutdown();
}

static void test_deflate_disabled_fallback(void* arg) {
    (void)arg;
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_handler, NULL, wg);

    xylem_ws_opts_t  client_opts = {.permessage_deflate = true};
    xylem_ws_conn_t* c           = _connect(l, &client_opts);

    const char* text = "no compression here";
    _echo_roundtrip(c, XYLEM_WS_TEXT, text, strlen(text));

    _drain(c, l, wg);
    xylem_shutdown();
}

typedef void (*test_fn_t)(void*);

static test_fn_t tests[] = {
    test_null_guards,
    test_text_echo,
    test_binary_echo,
    test_multiple_messages,
    test_large_message,
    test_server_close,
    test_deflate_text_echo,
    test_deflate_context_takeover,
    test_deflate_large_binary,
    test_deflate_disabled_fallback,
};

static int test_count = (int)(sizeof(tests) / sizeof(tests[0]));

int main(void) {
    for (int i = 0; i < test_count; i++) {
        xylem_run(tests[i], NULL, NULL);
    }
    printf("All %d WS tests passed.\n", test_count);
    return 0;
}
