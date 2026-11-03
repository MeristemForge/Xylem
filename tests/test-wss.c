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
#define TEST_WITH_TLS
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WSS_CERT "xylem-test-wss-cert.pem"
#define WSS_KEY  "xylem-test-wss-key.pem"

typedef void (*_test_fn_t)(void*);
typedef void (*_handler_fn_t)(xylem_ws_conn_t*, void*);

typedef struct {
    xylem_ws_listener_t* listener;
    _handler_fn_t        handler;
    xylem_waitgroup_t*   wg;
} _accept_ctx_t;

static const xylem_ws_tls_t _wss_srv_tls = {.cert = WSS_CERT, .key = WSS_KEY};

static const xylem_ws_tls_t _wss_cli_tls = {.skip_verify = true};

static void _srv_echo_cb(xylem_ws_conn_t* ws, void* ud) {
    uint8_t* buf = (uint8_t*)malloc(65536);
    ASSERT(buf);
    xylem_ws_opcode_t type;
    int               len;
    while ((len = xylem_ws_recv(ws, buf, 65536, &type)) >= 0) {
        ASSERT(xylem_ws_send(ws, buf, len, type) == 0);
    }
    free(buf);
    xylem_ws_close(ws, 1000, NULL, 0);
    xylem_ws_destroy(ws);
    xylem_waitgroup_done((xylem_waitgroup_t*)ud);
}

static void _srv_accept_cb(void* arg) {
    _accept_ctx_t*       ctx      = arg;
    xylem_ws_listener_t* listener = ctx->listener;
    _handler_fn_t        handler  = ctx->handler;
    xylem_waitgroup_t*   wg       = ctx->wg;
    free(ctx);
    xylem_ws_conn_t* conn = xylem_ws_accept(listener);
    if (conn) {
        handler(conn, wg);
    } else {
        xylem_waitgroup_done(wg);
    }
}

static uint16_t _listener_port(xylem_ws_listener_t* listener) {
    uint16_t port = 0;
    ASSERT(xylem_ws_listener_addr(listener, NULL, 0, &port) == 0);
    return port;
}

static xylem_ws_listener_t* _listen(
    const xylem_ws_opts_t* opts,
    xylem_waitgroup_t*     wg) {
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0, opts);
    ASSERT(l != NULL);
    _accept_ctx_t* ctx = (_accept_ctx_t*)malloc(sizeof(_accept_ctx_t));
    ASSERT(ctx);
    *ctx = (_accept_ctx_t){.listener = l, .handler = _srv_echo_cb, .wg = wg};
    xylem_waitgroup_add(wg, 1);
    xylem_spawn(_srv_accept_cb, ctx);
    return l;
}

static void _drain(
    xylem_ws_conn_t* c,
    xylem_ws_listener_t* l,
    xylem_waitgroup_t* wg) {
    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_destroy(c);
    xylem_waitgroup_wait(wg);
    xylem_ws_close_listener(l);
    xylem_ws_destroy_listener(l);
    xylem_waitgroup_destroy(wg);
}

static xylem_ws_conn_t* _connect(
    xylem_ws_listener_t*   l,
    const xylem_ws_opts_t* cli_opts) {
    uint16_t port = _listener_port(l);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%u/", port);

    xylem_ws_conn_t* c = xylem_ws_dial(url, cli_opts);
    ASSERT(c != NULL);
    return c;
}

static void _echo_roundtrip(
    xylem_ws_conn_t*  c,
    xylem_ws_opcode_t opcode,
    const void*       data,
    size_t            len) {
    uint8_t* buf = (uint8_t*)malloc(len + 1);
    ASSERT(buf);
    memset(buf, 0xA5, len + 1);
    ASSERT(xylem_ws_send(c, data, (int)len, opcode) == 0);
    xylem_ws_opcode_t type;
    ASSERT(xylem_ws_recv(c, buf, (int)len, &type) == (int)len);
    ASSERT(type == opcode);
    ASSERT(memcmp(buf, data, len) == 0);
    ASSERT(buf[len] == 0xA5);
    free(buf);
}

static void _accept_cancel_cb(void* arg) {
    _accept_ctx_t* ctx = arg;
    ASSERT(xylem_ws_accept(ctx->listener) == NULL);
    xylem_waitgroup_done(ctx->wg);
}

static void _listener_cancel_timer_cb(xylem_timer_t* timer, void* arg) {
    (void)timer;
    _accept_ctx_t* ctx = arg;
    xylem_ws_close_listener(ctx->listener);
    xylem_waitgroup_done(ctx->wg);
}

static void test_text_echo(void* arg) {
    (void)arg;

    xylem_ws_opts_t      srv_opts = {.tls = &_wss_srv_tls};
    xylem_waitgroup_t*   wg       = xylem_waitgroup_create();
    xylem_ws_listener_t* l        = _listen(&srv_opts, wg);

    xylem_ws_opts_t  cli_opts = {.tls = &_wss_cli_tls};
    xylem_ws_conn_t* c        = _connect(l, &cli_opts);

    const char* text = "hello secure websocket";
    _echo_roundtrip(c, XYLEM_WS_TEXT, text, strlen(text));

    _drain(c, l, wg);
}

static void test_large_message(void* arg) {
    (void)arg;

    xylem_ws_opts_t      srv_opts = {.fragment_threshold = 1024,
                                     .tls                = &_wss_srv_tls};
    xylem_waitgroup_t*   wg       = xylem_waitgroup_create();
    xylem_ws_listener_t* l        = _listen(&srv_opts, wg);

    xylem_ws_opts_t  cli_opts = {.fragment_threshold = 1024,
                                 .tls                = &_wss_cli_tls};
    xylem_ws_conn_t* c        = _connect(l, &cli_opts);

    size_t   big_len = 8192;
    uint8_t* big     = (uint8_t*)malloc(big_len);
    ASSERT(big != NULL);
    for (size_t i = 0; i < big_len; i++) {
        big[i] = (uint8_t)(i & 0xFF);
    }

    _echo_roundtrip(c, XYLEM_WS_BINARY, big, big_len);

    free(big);
    _drain(c, l, wg);
}

static void test_deflate(void* arg) {
    (void)arg;

    xylem_ws_opts_t srv_opts = {
        .permessage_deflate = true,
        .tls                = &_wss_srv_tls,
    };
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(&srv_opts, wg);

    xylem_ws_opts_t cli_opts = {
        .permessage_deflate = true,
        .tls                = &_wss_cli_tls,
    };
    xylem_ws_conn_t* c = _connect(l, &cli_opts);

    const char* text = "compressed payload over a TLS websocket connection!";
    _echo_roundtrip(c, XYLEM_WS_TEXT, text, strlen(text));

    _drain(c, l, wg);
}

static void test_close_listener_with_connection(void* arg) {
    (void)arg;
    xylem_ws_opts_t      opts   = {.tls = &_wss_srv_tls};
    xylem_waitgroup_t*   wg     = xylem_waitgroup_create();
    xylem_ws_listener_t* l      = _listen(&opts, wg);
    xylem_ws_opts_t      client = {.tls = &_wss_cli_tls};
    xylem_ws_conn_t*     c      = _connect(l, &client);
    xylem_ws_close_listener(l);
    xylem_ws_destroy_listener(l);
    _echo_roundtrip(c, XYLEM_WS_TEXT, "still connected", 15);
    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_destroy(c);
    xylem_waitgroup_wait(wg);
    xylem_waitgroup_destroy(wg);
}

static void test_deadlines_and_addresses(void* arg) {
    (void)arg;
    xylem_ws_opts_t    opts = {.tls = &_wss_srv_tls};
    xylem_waitgroup_t* wg   = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l      = _listen(&opts, wg);
    xylem_ws_opts_t      client = {.tls = &_wss_cli_tls};
    xylem_ws_conn_t*     c      = _connect(l, &client);
    char                 host[64];
    uint16_t             port = 0;
    ASSERT(xylem_ws_remote_addr(c, host, sizeof(host), &port) == 0);
    ASSERT(port == _listener_port(l));
    ASSERT(!strcmp(host, "127.0.0.1"));
    ASSERT(xylem_ws_local_addr(c, host, sizeof(host), &port) == 0);
    ASSERT(port);
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    xylem_ws_set_read_deadline(c, now - 1);
    xylem_ws_set_write_deadline(c, now - 1);
    xylem_ws_set_read_deadline(c, 0);
    xylem_ws_set_write_deadline(c, 0);
    _echo_roundtrip(c, XYLEM_WS_TEXT, "cleared", 7);
    xylem_ws_set_read_deadline(c,
                               xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) +
                                   40);
    char buf[16] = "unchanged";
    ASSERT(xylem_ws_recv(c, buf, sizeof(buf), NULL) == -1);
    xylem_ws_set_read_deadline(c, 0);
    ASSERT(xylem_ws_send(c, "x", 1, XYLEM_WS_TEXT) == -1);
    _drain(c, l, wg);
}

static void test_write_timeout(void* arg) {
    (void)arg;
    xylem_ws_opts_t    opts = {.tls = &_wss_srv_tls};
    xylem_waitgroup_t* wg   = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l      = _listen(&opts, wg);
    xylem_ws_opts_t      client = {.tls = &_wss_cli_tls};
    xylem_ws_conn_t*     c      = _connect(l, &client);
    xylem_ws_set_write_deadline(c,
                                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) -
                                    1);
    ASSERT(xylem_ws_ping(c, NULL, 0) == -1);
    xylem_ws_set_write_deadline(c, 0);
    ASSERT(xylem_ws_send(c, "x", 1, XYLEM_WS_TEXT) == -1);
    _drain(c, l, wg);
}

static void test_cancel_tls_handshake(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts = {.tls = &_wss_srv_tls, .handshake_timeout_ms = 5000};
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0, &opts);
    ASSERT(l);
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    _accept_ctx_t ctx = {.listener = l, .wg = wg};
    xylem_waitgroup_add(wg, 2);
    xylem_spawn(_accept_cancel_cb, &ctx);
    xylem_tcp_conn_t* peer =
        xylem_tcp_dial("127.0.0.1", _listener_port(l), NULL);
    ASSERT(peer);
    xylem_timer_t* timer =
        xylem_timer_after(40, _listener_cancel_timer_cb, &ctx);
    ASSERT(timer);
    xylem_tcp_set_read_deadline(peer,
                                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) +
                                    1000);
    char byte;
    ASSERT(xylem_tcp_read(peer, &byte, 1) <= 0);
    xylem_waitgroup_wait(wg);
    xylem_timer_destroy(timer);
    xylem_tcp_destroy(peer);
    xylem_ws_destroy_listener(l);
    xylem_waitgroup_destroy(wg);
}

static void test_short_buffer_retry(void* arg) {
    (void)arg;
    xylem_ws_opts_t    opts = {.tls                      = &_wss_srv_tls,
                               .fragment_threshold       = 3,
                               .permessage_deflate       = true,
                               .deflate_context_takeover = true};
    xylem_waitgroup_t* wg   = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l      = _listen(&opts, wg);
    xylem_ws_opts_t      client = opts;
    client.tls                  = &_wss_cli_tls;
    xylem_ws_conn_t* c          = _connect(l, &client);
    ASSERT(xylem_ws_send(c, "secure retry", 12, XYLEM_WS_TEXT) == 0);
    const uint8_t binary[] = {0x00, 0xFF, 0x02};
    ASSERT(xylem_ws_send(c, binary, sizeof(binary), XYLEM_WS_BINARY) == 0);
    char              small[4] = "keep";
    xylem_ws_opcode_t type     = XYLEM_WS_BINARY;
    for (int retry = 0; retry < 2; ++retry) {
        ASSERT(xylem_ws_recv(c, small, sizeof(small), &type) == -1);
        ASSERT(!memcmp(small, "keep", sizeof(small)));
        ASSERT(type == XYLEM_WS_BINARY);
    }
    char buf[12];
    ASSERT(xylem_ws_recv(c, buf, sizeof(buf), &type) == sizeof(buf));
    ASSERT(type == XYLEM_WS_TEXT);
    ASSERT(!memcmp(buf, "secure retry", sizeof(buf)));
    ASSERT(xylem_ws_recv(c, buf, sizeof(buf), &type) == sizeof(binary));
    ASSERT(type == XYLEM_WS_BINARY);
    ASSERT(!memcmp(buf, binary, sizeof(binary)));
    _drain(c, l, wg);
}

static void test_empty_messages(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts  = {.tls = &_wss_srv_tls, .permessage_deflate = true};
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l = _listen(&opts, wg);
    xylem_ws_opts_t client = {.tls = &_wss_cli_tls, .permessage_deflate = true};
    xylem_ws_conn_t* c     = _connect(l, &client);
    ASSERT(xylem_ws_send(c, NULL, 0, XYLEM_WS_TEXT) == 0);
    xylem_ws_opcode_t type = XYLEM_WS_BINARY;
    ASSERT(xylem_ws_recv(c, NULL, 0, &type) == 0);
    ASSERT(type == XYLEM_WS_TEXT);
    ASSERT(xylem_ws_send(c, NULL, 0, XYLEM_WS_BINARY) == 0);
    ASSERT(xylem_ws_recv(c, NULL, 0, NULL) == 0);
    _echo_roundtrip(c, XYLEM_WS_TEXT, "after empty", 11);
    _drain(c, l, wg);
}

static void _test_run_all_cb(void* arg) {
    (void)arg;
    const _test_fn_t tests[] = {
        test_short_buffer_retry,
        test_empty_messages,
        test_cancel_tls_handshake,
        test_deadlines_and_addresses,
        test_write_timeout,
        test_text_echo,
        test_large_message,
        test_deflate,
        test_close_listener_with_connection,
    };
    const size_t test_count = sizeof(tests) / sizeof(tests[0]);
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ASSERT(_utils_cert_gen(WSS_CERT, WSS_KEY) == 0);

    for (size_t i = 0; i < test_count; i++) {
        tests[i](NULL);
    }
    remove(WSS_CERT);
    remove(WSS_KEY);
    printf("All %zu WSS tests passed.\n", test_count);
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all_cb, NULL, NULL);
    return 0;
}
