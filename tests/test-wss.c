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
 * WSS (WebSocket over TLS) integration tests. test-ws.c covers plain ws://
 * and test-tls.c covers the raw TLS engine; this file covers the wss path:
 * a TLS-backed WebSocket listener, wss:// scheme dispatch in xylem_ws_dial,
 * and frame exchange (text/binary/large/permessage-deflate) over TLS.
 */

#include "xylem.h"
#include "xylem/net/xylem-ws.h"
#include "assert.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helpers. */

#define WSS_CERT "test_wss_cert.pem"
#define WSS_KEY  "test_wss_key.pem"

static const xylem_ws_tls_t _srv_tls = { .cert = WSS_CERT, .key = WSS_KEY };
static const xylem_ws_tls_t _cli_tls = { .skip_verify = true };

static void _srv_echo_handler(xylem_ws_conn_t* ws, void* ud) {
    (void)ud;
    xylem_ws_msg_t msg;
    while (xylem_ws_recv(ws, &msg) == 0) {
        xylem_ws_send(ws, msg.opcode, msg.data, msg.len);
        xylem_ws_msg_free(&msg);
    }
    xylem_ws_close(ws, 1000, NULL, 0);
}

/* Test: wss text echo. */

static void test_wss_text_echo(void* arg) {
    (void)arg;
    ASSERT(_cert_gen(WSS_CERT, WSS_KEY) == 0);

    xylem_ws_opts_t srv_opts = { .tls = &_srv_tls };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                             _srv_echo_handler, NULL, &srv_opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%u/", port);

    xylem_ws_opts_t cli_opts = { .tls = &_cli_tls };
    xylem_ws_conn_t* c = xylem_ws_dial(url, &cli_opts);
    ASSERT(c != NULL);

    const char* text = "hello secure websocket";
    ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, text, strlen(text)) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_TEXT);
    ASSERT(msg.len == strlen(text));
    ASSERT(memcmp(msg.data, text, msg.len) == 0);
    xylem_ws_msg_free(&msg);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    remove(WSS_CERT);
    remove(WSS_KEY);
    xylem_shutdown();
}

/* Test: wss large fragmented message. */

static void test_wss_large_message(void* arg) {
    (void)arg;
    ASSERT(_cert_gen(WSS_CERT, WSS_KEY) == 0);

    xylem_ws_opts_t srv_opts = { .fragment_threshold = 1024, .tls = &_srv_tls };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                             _srv_echo_handler, NULL, &srv_opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%u/", port);
    xylem_ws_opts_t cli_opts = { .fragment_threshold = 1024, .tls = &_cli_tls };
    xylem_ws_conn_t* c = xylem_ws_dial(url, &cli_opts);
    ASSERT(c != NULL);

    size_t big_len = 8192;
    uint8_t* big = (uint8_t*)malloc(big_len);
    ASSERT(big != NULL);
    for (size_t i = 0; i < big_len; i++) {
        big[i] = (uint8_t)(i & 0xFF);
    }

    ASSERT(xylem_ws_send(c, XYLEM_WS_BINARY, big, big_len) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_BINARY);
    ASSERT(msg.len == big_len);
    ASSERT(memcmp(msg.data, big, big_len) == 0);
    xylem_ws_msg_free(&msg);

    free(big);
    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    remove(WSS_CERT);
    remove(WSS_KEY);
    xylem_shutdown();
}

/* Test: wss with permessage-deflate. */

static void test_wss_deflate(void* arg) {
    (void)arg;
    ASSERT(_cert_gen(WSS_CERT, WSS_KEY) == 0);

    xylem_ws_opts_t srv_opts = {
        .permessage_deflate = true,
        .tls                = &_srv_tls,
    };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                             _srv_echo_handler, NULL, &srv_opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%u/", port);
    xylem_ws_opts_t cli_opts = {
        .permessage_deflate = true,
        .tls                = &_cli_tls,
    };
    xylem_ws_conn_t* c = xylem_ws_dial(url, &cli_opts);
    ASSERT(c != NULL);

    const char* text = "compressed payload over a TLS websocket connection!";
    ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, text, strlen(text)) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_TEXT);
    ASSERT(msg.len == strlen(text));
    ASSERT(memcmp(msg.data, text, msg.len) == 0);
    xylem_ws_msg_free(&msg);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    remove(WSS_CERT);
    remove(WSS_KEY);
    xylem_shutdown();
}

/* Runner. */

typedef void (*test_fn_t)(void*);

static test_fn_t tests[] = {
    test_wss_text_echo,
    test_wss_large_message,
    test_wss_deflate,
};

static int test_count = (int)(sizeof(tests) / sizeof(tests[0]));

int main(void) {
    for (int i = 0; i < test_count; i++) {
        xylem_run(tests[i], NULL, NULL);
    }
    printf("All %d WSS tests passed.\n", test_count);
    return 0;
}
