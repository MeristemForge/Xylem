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
#include "assert.h"

#include "xylem/net/xylem-ws.h"

#include "net/ws/ws-frame.h"
#include "net/ws/ws-handshake.h"
#include "net/ws/ws-utils.h"
#include "net/ws/ws.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char        response[1024];
    size_t      len, offset;
    bool        built, closed, invalid;
    uint64_t    read_deadline, write_deadline;
    uint64_t    first_read_deadline, first_write_deadline;
    const char* expected_host_header;
    bool        expect_extensions;
} _handshake_peer_t;

typedef struct {
    const char* request;
    size_t      len, offset;
    char        response[512];
    size_t      writes, closes, destroys;
    bool        fail_write;
    uint64_t    read_deadline, write_deadline;
} _server_handshake_peer_t;

typedef void (*_test_fn_t)(void*);
typedef void (*_handler_fn_t)(xylem_ws_conn_t*, void*);

typedef struct {
    xylem_ws_listener_t* listener;
    _handler_fn_t        handler;
    xylem_waitgroup_t*   wg;
} _accept_ctx_t;

typedef struct {
    xylem_ws_listener_t* listener;
    xylem_waitgroup_t*   wg;
} _cancel_accept_t;

typedef struct {
    xylem_ws_conn_t*   conn;
    xylem_waitgroup_t* wg;
} _close_read_t;

static const char* const _upgrade_request =
    "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
    "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Key: "
    "dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n";


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
    _handler_fn_t          handler,
    const xylem_ws_opts_t* opts,
    xylem_waitgroup_t*     wg) {
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0, opts);
    ASSERT(l != NULL);
    _accept_ctx_t* ctx = (_accept_ctx_t*)malloc(sizeof(_accept_ctx_t));
    ASSERT(ctx);
    *ctx = (_accept_ctx_t){.listener = l, .handler = handler, .wg = wg};
    xylem_waitgroup_add(wg, 1);
    xylem_spawn(_srv_accept_cb, ctx);
    return l;
}

static void
_drain(xylem_ws_conn_t* c, xylem_ws_listener_t* l, xylem_waitgroup_t* wg) {
    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_destroy(c);
    xylem_waitgroup_wait(wg);
    xylem_ws_close_listener(l);
    xylem_ws_destroy_listener(l);
    xylem_waitgroup_destroy(wg);
}

static xylem_ws_conn_t*
_connect(xylem_ws_listener_t* l, const xylem_ws_opts_t* cli_opts) {
    uint16_t port = _listener_port(l);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);

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

static void test_message_roundtrips(void* arg) {
    (void)arg;
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_ws_listener_t* listener = _listen(_srv_echo_cb, NULL, wg);
    xylem_ws_conn_t*     conn     = _connect(listener, NULL);

    const char* text = "hello websocket";
    _echo_roundtrip(conn, XYLEM_WS_TEXT, text, strlen(text));
    const uint8_t binary[] = {0x00, 0x01, 0x02, 0xff, 0xfe};
    _echo_roundtrip(conn, XYLEM_WS_BINARY, binary, sizeof(binary));
    for (int i = 0; i < 10; i++) {
        char message[32];
        int  len = snprintf(message, sizeof(message), "msg-%d", i);
        ASSERT(len > 0 && (size_t)len < sizeof(message));
        _echo_roundtrip(conn, XYLEM_WS_TEXT, message, (size_t)len);
    }
    _drain(conn, listener, wg);
}

static void test_large_message(void* arg) {
    (void)arg;
    xylem_ws_opts_t      opts = {.fragment_threshold = 1024};
    xylem_waitgroup_t*   wg   = xylem_waitgroup_create();
    xylem_ws_listener_t* l    = _listen(_srv_echo_cb, &opts, wg);

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
}

static void _srv_close_cb(xylem_ws_conn_t* ws, void* ud) {
    char buf[32];
    ASSERT(xylem_ws_recv(ws, buf, sizeof(buf), NULL) == 7);
    xylem_ws_close(ws, 1000, "bye", 3);
    xylem_ws_destroy(ws);
    xylem_waitgroup_done((xylem_waitgroup_t*)ud);
}

static void test_server_close(void* arg) {
    (void)arg;
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_close_cb, NULL, wg);

    xylem_ws_conn_t* c = _connect(l, NULL);
    ASSERT(xylem_ws_send(c, "trigger", 7, XYLEM_WS_TEXT) == 0);

    char buf[16] = "unchanged";
    ASSERT(xylem_ws_recv(c, buf, sizeof(buf), NULL) == -1);

    _drain(c, l, wg);
}

static void test_url_parsing(void* arg) {
    (void)arg;
    const struct {
        const char* url;
        const char* host;
        const char* path;
        uint16_t    port;
        bool        secure;
    } cases[] = {
        {"ws://Example.COM", "example.com", "/", 80, false},
        {"wss://Example.COM/chat?token=a%20b",
         "example.com",
         "/chat?token=a%20b",
         443,
         true},
        {"ws://localhost:8080?room=1", "localhost", "/?room=1", 8080, false},
        {"wss://[::1]:8443/chat", "::1", "/chat", 8443, true},
        {"ws://[::1]", "::1", "/", 80, false},
        {"ws://localhost/chat#fragment", "localhost", "/chat", 80, false},
    };
    ws_url_t parsed;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ASSERT(ws_url_parse(cases[i].url, &parsed) == 0);
        ASSERT(!strcmp(parsed.host, cases[i].host));
        ASSERT(!strcmp(parsed.path, cases[i].path));
        ASSERT(parsed.port == cases[i].port);
        ASSERT(parsed.secure == cases[i].secure);
    }
    const char* invalid[] = {
        "http://localhost",
        "https://localhost",
        "ws://",
        "ws:///chat",
        "ws://user@localhost",
        "ws://local\\host",
        "ws://localhost:0",
        "ws://localhost:65536",
        "ws://localhost:-1",
        "ws://localhost:abc",
        "ws://localhost:",
        "ws://[::1",
        "ws://[::1]extra",
        "ws://localhost/a b",
        "ws://localhost/\r\nInjected:yes",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        ASSERT(ws_url_parse(invalid[i], &parsed) == -1);
        ASSERT(xylem_ws_dial(invalid[i], NULL) == NULL);
    }
    ASSERT(ws_url_parse(NULL, &parsed) == -1);
    ASSERT(ws_url_parse("ws://localhost", NULL) == -1);

    char long_url[4096];
    memcpy(long_url, "ws://", 5);
    memset(long_url + 5, 'a', 256);
    long_url[261] = 0;
    ASSERT(ws_url_parse(long_url, &parsed) == -1);
    ASSERT(xylem_ws_dial(long_url, NULL) == NULL);

    memcpy(long_url, "ws://localhost/", 15);
    memset(long_url + 15, 'a', 2047);
    long_url[2062] = 0;
    ASSERT(ws_url_parse(long_url, &parsed) == -1);
    ASSERT(xylem_ws_dial(long_url, NULL) == NULL);
}

static void test_null_guards(void* arg) {
    (void)arg;
    ASSERT(xylem_ws_dial(NULL, NULL) == NULL);
    ASSERT(xylem_ws_dial("http://bad", NULL) == NULL);
    ASSERT(xylem_ws_send(NULL, "x", 1, XYLEM_WS_TEXT) == -1);
    ASSERT(xylem_ws_recv(NULL, NULL, 0, NULL) == -1);
    ASSERT(xylem_ws_ping(NULL, NULL, 0) == -1);
    ASSERT(xylem_ws_close(NULL, 1000, NULL, 0) == -1);
    ASSERT(xylem_ws_get_userdata(NULL) == NULL);
    xylem_ws_set_userdata(NULL, NULL);
    xylem_ws_close_listener(NULL);
    xylem_ws_destroy_listener(NULL);
    ASSERT(xylem_ws_listener_addr(NULL, NULL, 0, NULL) == -1);
    ASSERT(xylem_ws_accept(NULL) == NULL);
    ASSERT(xylem_ws_remote_addr(NULL, NULL, 0, NULL) == -1);
    ASSERT(xylem_ws_local_addr(NULL, NULL, 0, NULL) == -1);
    xylem_ws_set_read_deadline(NULL, 0);
    xylem_ws_set_write_deadline(NULL, 0);
    xylem_ws_destroy(NULL);
}

static void test_deflate_text_echo(void* arg) {
    (void)arg;
    xylem_ws_opts_t      opts = {.permessage_deflate = true};
    xylem_waitgroup_t*   wg   = xylem_waitgroup_create();
    xylem_ws_listener_t* l    = _listen(_srv_echo_cb, &opts, wg);

    xylem_ws_conn_t* c    = _connect(l, &opts);
    const char*      text = "hello permessage-deflate compression test!";
    _echo_roundtrip(c, XYLEM_WS_TEXT, text, strlen(text));

    _drain(c, l, wg);
}

static void test_deflate_context_takeover(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts = {
        .permessage_deflate       = true,
        .deflate_context_takeover = true,
    };
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_cb, &opts, wg);

    xylem_ws_conn_t* c = _connect(l, &opts);
    for (int i = 0; i < 10; i++) {
        char buf[128];
        int  len = snprintf(
            buf,
            sizeof(buf),
            "message number %d with repeated content for compression",
            i);
        _echo_roundtrip(c, XYLEM_WS_TEXT, buf, (size_t)len);
    }

    _drain(c, l, wg);
}

static void test_deflate_large_binary(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts = {
        .permessage_deflate = true,
        .fragment_threshold = 4096,
    };
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_cb, &opts, wg);

    xylem_ws_conn_t* c = _connect(l, &opts);

    size_t   big_len = 32768;
    uint8_t* big     = (uint8_t*)malloc(big_len);
    ASSERT(big != NULL);
    memset(big, 'A', big_len);

    _echo_roundtrip(c, XYLEM_WS_BINARY, big, big_len);

    free(big);
    _drain(c, l, wg);
}

static void test_deflate_disabled_fallback(void* arg) {
    (void)arg;
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_cb, NULL, wg);

    xylem_ws_opts_t  client_opts = {.permessage_deflate = true};
    xylem_ws_conn_t* c           = _connect(l, &client_opts);

    const char* text = "no compression here";
    _echo_roundtrip(c, XYLEM_WS_TEXT, text, strlen(text));

    _drain(c, l, wg);
}

static void test_bad_handshakes(void* arg) {
    (void)arg;
    xylem_ws_opts_t      opts = {.handshake_timeout_ms = 40};
    xylem_waitgroup_t*   wg   = xylem_waitgroup_create();
    xylem_ws_listener_t* l    = _listen(_srv_echo_cb, &opts, wg);
    ASSERT(l);
    const char* requests[] = {
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: "
        "websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: invalid\r\nSec-WebSocket-Version: 13\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: "
        "websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: "
        "12\r\n\r\n",
        "GET / HTTP/1.1\r\n"};
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        xylem_tcp_conn_t* c =
            xylem_tcp_dial("127.0.0.1", _listener_port(l), NULL);
        ASSERT(c);
        ASSERT(xylem_tcp_write(c, requests[i], (int)strlen(requests[i])) == 0);
        xylem_tcp_set_read_deadline(
            c,
            xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 1000);
        char buf[64];
        ASSERT(xylem_tcp_read(c, buf, sizeof(buf)) == 0);
        xylem_tcp_destroy(c);
    }
    xylem_ws_conn_t* c = _connect(l, NULL);
    _echo_roundtrip(c, XYLEM_WS_TEXT, "after invalid peers", 19);
    _drain(c, l, wg);
}

static void test_upgrade_with_first_frame(void* arg) {
    (void)arg;
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_cb, NULL, wg);
    xylem_tcp_conn_t* c = xylem_tcp_dial("127.0.0.1", _listener_port(l), NULL);
    ASSERT(c);
    uint8_t packet[1024];
    size_t  n = strlen(_upgrade_request);
    memcpy(packet, _upgrade_request, n);
    uint8_t mask[4] = {1, 2, 3, 4};
    n += ws_frame_encode_header(packet + n, true, 1, true, mask, 2);
    packet[n++] = (uint8_t)('h' ^ mask[0]);
    packet[n++] = (uint8_t)('i' ^ mask[1]);
    ASSERT(xylem_tcp_write(c, packet, (int)n) == 0);
    xylem_tcp_set_read_deadline(
        c,
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 1000);
    char   reply[2048];
    size_t have = 0;
    char*  end  = NULL;
    for (;;) {
        int got =
            xylem_tcp_read(c, reply + have, (int)(sizeof(reply) - 1 - have));
        ASSERT(got > 0);
        have += (size_t)got;
        reply[have] = 0;
        end         = strstr(reply, "\r\n\r\n");
        if (end && have >= (size_t)(end + 4 - reply) + 4) {
            break;
        }
    }
    ASSERT(strstr(reply, "101 Switching Protocols"));
    ws_frame_header_t frame;
    ASSERT(
        ws_frame_decode_header(
            (uint8_t*)end + 4,
            have - (size_t)(end + 4 - reply),
            &frame) == 0);
    ASSERT(frame.payload_len == 2);
    ASSERT(!memcmp(end + 4 + frame.header_size, "hi", 2));
    xylem_tcp_destroy(c);
    xylem_waitgroup_wait(wg);
    xylem_ws_close_listener(l);
    xylem_ws_destroy_listener(l);
    xylem_waitgroup_destroy(wg);
}

static void test_close_listener_with_connection(void* arg) {
    (void)arg;
    xylem_waitgroup_t*   wg = xylem_waitgroup_create();
    xylem_ws_listener_t* l  = _listen(_srv_echo_cb, NULL, wg);
    xylem_ws_conn_t*     c  = _connect(l, NULL);
    xylem_ws_close_listener(l);
    xylem_ws_destroy_listener(l);
    _echo_roundtrip(c, XYLEM_WS_TEXT, "still connected", 15);
    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_destroy(c);
    xylem_waitgroup_wait(wg);
    xylem_waitgroup_destroy(wg);
}

static int _cli_peer_write_cb(void* arg, const void* data, int len) {
    _handshake_peer_t* p = arg;
    if (!p->built) {
        char request[2048];
        ASSERT(len < (int)sizeof(request));
        memcpy(request, data, (size_t)len);
        request[len] = 0;
        if (p->expected_host_header) {
            const char* line = "GET /chat?room=1 HTTP/1.1\r\n";
            ASSERT(!strncmp(request, line, strlen(line)));
            ASSERT(strstr(request, p->expected_host_header));
            ASSERT(
                (strstr(request, "Sec-WebSocket-Extensions:") != NULL) ==
                p->expect_extensions);
            ASSERT(!memcmp(request + len - 4, "\r\n\r\n", 4));
        }
        char* value = strstr(request, "Sec-WebSocket-Key: ");
        ASSERT(value);
        char key[25], accept[29];
        memcpy(key, value + 19, 24);
        key[24] = 0;
        ASSERT(!ws_handshake_compute_accept(key, accept, sizeof(accept)));
        int n = snprintf(
            p->response,
            sizeof(p->response),
            "HTTP/1.1 101 Switching Protocols\r\nuPgRaDe: websocket\r\n"
            "cOnNeCtIoN: %s\r\nsEc-WeBsOcKeT-aCcEpT: %s\r\n\r\n",
            p->invalid ? "close" : "keep-alive, Upgrade",
            accept);
        p->len                = (size_t)n;
        p->response[p->len++] = (char)0x81;
        p->response[p->len++] = 2;
        p->response[p->len++] = 'h';
        p->response[p->len++] = 'i';
        p->built              = true;
    }
    return 0;
}

static int _cli_peer_read_cb(void* arg, void* data, int cap) {
    _handshake_peer_t* p = arg;
    size_t             n = p->len - p->offset;
    if (!p->offset && n > 75) {
        n = 75; /* Split the accept header across reads. */
    }
    if (n > (size_t)cap) {
        n = (size_t)cap;
    }
    if (!n) {
        return 0;
    }
    memcpy(data, p->response + p->offset, n);
    p->offset += n;
    return (int)n;
}

static void _cli_peer_close_cb(void* arg) {
    ((_handshake_peer_t*)arg)->closed = true;
}

static void _cli_peer_read_deadline_cb(void* arg, uint64_t ms) {
    _handshake_peer_t* p = arg;
    p->read_deadline     = ms;
    if (ms && !p->first_read_deadline) {
        p->first_read_deadline = ms;
    }
}

static void _cli_peer_write_deadline_cb(void* arg, uint64_t ms) {
    _handshake_peer_t* p = arg;
    p->write_deadline    = ms;
    if (ms && !p->first_write_deadline) {
        p->first_write_deadline = ms;
    }
}

static void test_handshake_accept_vector(void* arg) {
    (void)arg;
    /* RFC 6455 section 1.3: independently specified Key/Accept pair. */
    const char* key      = "dGhlIHNhbXBsZSBub25jZQ==";
    const char* expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    char        accept[29];
    ASSERT(ws_handshake_compute_accept(key, accept, sizeof(accept)) == 0);
    ASSERT(!strcmp(accept, expected));
    ASSERT(ws_handshake_validate_accept(key, expected) == 0);
    ASSERT(
        ws_handshake_validate_accept(key, "a6mgPHlqzRgZe/6MrAQKXgJ7ACc=") ==
        -1);
}

static void test_client_handshake_request_variants(void* arg) {
    (void)arg;
    const struct {
        uint16_t    port;
        const char* host_header;
        bool        extensions;
    } cases[] = {
        {.port = 80, .host_header = "Host: localhost\r\n"},
        {.port = 443, .host_header = "Host: localhost\r\n"},
        {.port = 8080, .host_header = "Host: localhost:8080\r\n"},
        {.port        = 8080,
         .host_header = "Host: localhost:8080\r\n",
         .extensions  = true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        _handshake_peer_t peer = {
            .expected_host_header = cases[i].host_header,
            .expect_extensions    = cases[i].extensions};
        ws_transport_t t = {
            .conn            = &peer,
            .read            = _cli_peer_read_cb,
            .write           = _cli_peer_write_cb,
            .close           = _cli_peer_close_cb,
            .destroy         = _cli_peer_close_cb,
            .set_rd_deadline = _cli_peer_read_deadline_cb,
            .set_wr_deadline = _cli_peer_write_deadline_cb};
        xylem_ws_opts_t opts = {.permessage_deflate = cases[i].extensions};
        ws_handshake_result_t result;
        ASSERT(
            ws_handshake_client(
                &t,
                "localhost",
                cases[i].port,
                "/chat?room=1",
                &opts,
                &result) == 0);
        ASSERT(!peer.closed);
        ASSERT(peer.built);
        t.destroy(t.conn);
    }
}

static void test_client_handshake_borrows_transport(void* arg) {
    (void)arg;
    for (int invalid = 0; invalid < 2; ++invalid) {
        _handshake_peer_t peer = {.invalid = invalid != 0};
        ws_transport_t    t    = {
            .conn            = &peer,
            .read            = _cli_peer_read_cb,
            .write           = _cli_peer_write_cb,
            .close           = _cli_peer_close_cb,
            .destroy         = _cli_peer_close_cb,
            .set_rd_deadline = _cli_peer_read_deadline_cb,
            .set_wr_deadline = _cli_peer_write_deadline_cb};
        ws_handshake_result_t result;
        ASSERT(
            ws_handshake_client(&t, "localhost", 80, "/", NULL, &result) ==
            (invalid ? -1 : 0));
        ASSERT(!peer.closed);
        ASSERT(peer.first_write_deadline != 0);
        ASSERT(peer.first_read_deadline == peer.first_write_deadline);
        if (!invalid) {
            const uint8_t frame[] = {0x81, 2, 'h', 'i'};
            ASSERT(result.pending_len == sizeof(frame));
            ASSERT(!memcmp(result.pending, frame, sizeof(frame)));
            ASSERT(!result.deflate);
            ASSERT(peer.read_deadline == 0 && peer.write_deadline == 0);
        }
        t.destroy(t.conn);
        ASSERT(peer.closed);
    }
}

static int _srv_peer_read_cb(void* arg, void* data, int cap) {
    _server_handshake_peer_t* p = arg;
    size_t                    n = p->len - p->offset;
    if (!p->offset && n > 75) {
        n = 75;
    }
    if (n > (size_t)cap) {
        n = (size_t)cap;
    }
    memcpy(data, p->request + p->offset, n);
    p->offset += n;
    return (int)n;
}

static int _srv_peer_write_cb(void* arg, const void* data, int len) {
    _server_handshake_peer_t* p = arg;
    ++p->writes;
    if (p->fail_write) {
        return -1;
    }
    ASSERT(len > 0 && (size_t)len < sizeof(p->response));
    memcpy(p->response, data, (size_t)len);
    p->response[len] = 0;
    return 0;
}

static void _srv_peer_close_cb(void* arg) {
    ++((_server_handshake_peer_t*)arg)->closes;
}

static void _srv_peer_destroy_cb(void* arg) {
    ++((_server_handshake_peer_t*)arg)->destroys;
}

static void _srv_peer_read_deadline_cb(void* arg, uint64_t ms) {
    ((_server_handshake_peer_t*)arg)->read_deadline = ms;
}

static void _srv_peer_write_deadline_cb(void* arg, uint64_t ms) {
    ((_server_handshake_peer_t*)arg)->write_deadline = ms;
}

static ws_transport_t _srv_peer_transport(_server_handshake_peer_t* peer) {
    return (ws_transport_t){
        .conn            = peer,
        .read            = _srv_peer_read_cb,
        .write           = _srv_peer_write_cb,
        .close           = _srv_peer_close_cb,
        .destroy         = _srv_peer_destroy_cb,
        .set_rd_deadline = _srv_peer_read_deadline_cb,
        .set_wr_deadline = _srv_peer_write_deadline_cb};
}

static void test_server_handshake_stages(void* arg) {
    (void)arg;
    const char request[] =
        "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: "
        "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; "
        "client_no_context_takeover\r\n\r\n"
        "\x81\x80\x01\x02\x03\x04";
    const uint8_t frame[] = {0x81, 0x80, 1, 2, 3, 4};
    for (int scenario = 0; scenario < 3; ++scenario) {
        _server_handshake_peer_t peer = {
            .request = request,
            .len     = sizeof(request) - 1};
        ws_transport_t  t    = _srv_peer_transport(&peer);
        xylem_ws_opts_t opts = {
            .permessage_deflate       = scenario != 2,
            .deflate_context_takeover = scenario == 0};
        ws_handshake_result_t result;
        ws_handshake_reply_t  reply;
        ASSERT(ws_handshake_server_prepare(&t, &opts, &result, &reply) == 0);
        ASSERT(peer.writes == 0);
        ASSERT(peer.read_deadline != 0);
        ASSERT(peer.read_deadline == peer.write_deadline);
        ASSERT(peer.read_deadline == reply.deadline);
        ASSERT(result.pending_len == sizeof(frame));
        ASSERT(!memcmp(result.pending, frame, sizeof(frame)));
        ASSERT(result.deflate == (scenario != 2));
        ASSERT(result.no_context_takeover == (scenario == 1));
        ASSERT(ws_handshake_server_finish(&t, &reply) == 0);
        ASSERT(peer.writes == 1);
        ASSERT(strstr(peer.response, "HTTP/1.1 101 Switching Protocols\r\n"));
        ASSERT(strstr(peer.response, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
        ASSERT(
            (strstr(peer.response, "Sec-WebSocket-Extensions:") != NULL) ==
            (scenario != 2));
        ASSERT(peer.read_deadline == 0 && peer.write_deadline == 0);
        ASSERT(peer.closes == 0 && peer.destroys == 0);
    }
}

static void test_server_handshake_failure_ownership(void* arg) {
    (void)arg;
    for (int scenario = 0; scenario < 3; ++scenario) {
        const char* request = scenario == 0
                                  ? "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
                                  : _upgrade_request;
        _server_handshake_peer_t peer = {
            .request    = request,
            .len        = strlen(request),
            .fail_write = scenario == 1};
        ws_transport_t t = _srv_peer_transport(&peer);
        if (scenario < 2) {
            ASSERT(ws_accept_impl(t, NULL) == NULL);
            ASSERT(peer.writes == (scenario == 1 ? 1u : 0u));
        } else {
            xylem_ws_opts_t       opts = {.handshake_timeout_ms = 20};
            ws_handshake_result_t result;
            ws_handshake_reply_t  reply;
            ASSERT(
                ws_handshake_server_prepare(&t, &opts, &result, &reply) == 0);
            xylem_sleep(30);
            ASSERT(ws_handshake_server_finish(&t, &reply) == -1);
            ASSERT(peer.writes == 0);
        }
        ASSERT(peer.closes == 0 && peer.destroys == 0);
        t.destroy(t.conn);
        ASSERT(peer.destroys == 1);
    }
}

static void test_client_handshake_parser(void* arg) {
    (void)arg;
    for (int scenario = 0; scenario < 4; ++scenario) {
        _handshake_peer_t peer = {.invalid = scenario == 1};
        ws_transport_t    t    = {
            .conn            = &peer,
            .read            = _cli_peer_read_cb,
            .write           = _cli_peer_write_cb,
            .close           = _cli_peer_close_cb,
            .destroy         = _cli_peer_close_cb,
            .set_rd_deadline = _cli_peer_read_deadline_cb,
            .set_wr_deadline = _cli_peer_write_deadline_cb};
        xylem_ws_conn_t* c = ws_dial_impl(t, "localhost", 80, "/", NULL);
        if (scenario == 1) {
            ASSERT(!c);
        } else if (scenario >= 2) {
            ASSERT(c);
            ASSERT(peer.read_deadline == 0 && peer.write_deadline == 0);
            char              buf[16] = "unchanged";
            xylem_ws_opcode_t type    = XYLEM_WS_BINARY;
            if (scenario == 3) {
                ASSERT(xylem_ws_recv(c, buf, 1, &type) == -1);
                ASSERT(!memcmp(buf, "unchanged", 10));
                ASSERT(type == XYLEM_WS_BINARY);
            }
            xylem_ws_set_read_deadline(
                c,
                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) - 1);
            ASSERT(xylem_ws_recv(c, buf, sizeof(buf), &type) == -1);
            ASSERT(!memcmp(buf, "unchanged", 10));
            ASSERT(type == XYLEM_WS_BINARY);
            xylem_ws_destroy(c);
        } else {
            ASSERT(c);
            ASSERT(peer.read_deadline == 0 && peer.write_deadline == 0);
            char buf[16] = "unchanged";
            ASSERT(xylem_ws_recv(c, buf, sizeof(buf), NULL) == 2);
            ASSERT(!memcmp(buf, "hi", 2));
            xylem_ws_close(c, 1000, NULL, 0);
            xylem_ws_destroy(c);
        }
        ASSERT(peer.closed);
    }
}

static void _accept_cancel_cb(void* arg) {
    _cancel_accept_t* ctx = arg;
    ASSERT(xylem_ws_accept(ctx->listener) == NULL);
    xylem_waitgroup_done(ctx->wg);
}

static void _listener_cancel_timer_cb(xylem_timer_t* timer, void* arg) {
    (void)timer;
    _cancel_accept_t* ctx = arg;
    xylem_ws_close_listener(ctx->listener);
    xylem_ws_close_listener(ctx->listener);
    xylem_waitgroup_done(ctx->wg);
}

static void test_cancel_accept(void* arg) {
    (void)arg;
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0, NULL);
    ASSERT(l);
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    _cancel_accept_t ctx = {.listener = l, .wg = wg};
    xylem_waitgroup_add(wg, 2);
    xylem_spawn(_accept_cancel_cb, &ctx);
    xylem_timer_t* timer =
        xylem_timer_after(40, _listener_cancel_timer_cb, &ctx);
    ASSERT(timer);
    xylem_waitgroup_wait(wg);
    xylem_timer_destroy(timer);
    ASSERT(xylem_ws_accept(l) == NULL);
    xylem_ws_destroy_listener(l);
    xylem_waitgroup_destroy(wg);
}

static void test_cancel_handshake(void* arg) {
    (void)arg;
    xylem_ws_opts_t      opts = {.handshake_timeout_ms = 5000};
    xylem_ws_listener_t* l    = xylem_ws_listen("127.0.0.1", 0, &opts);
    ASSERT(l);
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    _cancel_accept_t ctx = {.listener = l, .wg = wg};
    xylem_waitgroup_add(wg, 2);
    xylem_spawn(_accept_cancel_cb, &ctx);
    xylem_tcp_conn_t* peer =
        xylem_tcp_dial("127.0.0.1", _listener_port(l), NULL);
    ASSERT(peer);
    ASSERT(xylem_tcp_write(peer, "GET / HTTP/1.1\r\n", 16) == 0);
    xylem_timer_t* timer =
        xylem_timer_after(40, _listener_cancel_timer_cb, &ctx);
    ASSERT(timer);
    xylem_tcp_set_read_deadline(
        peer,
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 1000);
    char byte;
    ASSERT(xylem_tcp_read(peer, &byte, 1) <= 0);
    xylem_waitgroup_wait(wg);
    xylem_timer_destroy(timer);
    xylem_tcp_destroy(peer);
    xylem_ws_destroy_listener(l);
    xylem_waitgroup_destroy(wg);
}

static void test_addresses_and_deadline_clear(void* arg) {
    (void)arg;
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l = _listen(_srv_echo_cb, NULL, wg);
    xylem_ws_conn_t*     c = _connect(l, NULL);
    char                 host[64];
    uint16_t             remote = 0, local = 0;
    ASSERT(xylem_ws_remote_addr(c, host, sizeof(host), &remote) == 0);
    ASSERT(!strcmp(host, "127.0.0.1"));
    ASSERT(remote == _listener_port(l));
    ASSERT(xylem_ws_local_addr(c, host, sizeof(host), &local) == 0);
    ASSERT(local != 0);
    ASSERT(xylem_ws_remote_addr(c, host, 1, NULL) == -1);
    xylem_ws_set_userdata(c, wg);
    ASSERT(xylem_ws_get_userdata(c) == wg);
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    xylem_ws_set_read_deadline(c, now - 1);
    xylem_ws_set_write_deadline(c, now - 1);
    xylem_ws_set_read_deadline(c, 0);
    xylem_ws_set_write_deadline(c, 0);
    ASSERT(xylem_ws_ping(c, "ping", 4) == 0);
    _echo_roundtrip(c, XYLEM_WS_TEXT, "cleared", 7);
    xylem_ws_set_read_deadline(c, now + 2000);
    xylem_ws_set_write_deadline(c, now + 2000);
    _echo_roundtrip(c, XYLEM_WS_TEXT, "bounded", 7);
    _drain(c, l, wg);
}

static void test_read_timeout(void* arg) {
    (void)arg;
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l = _listen(_srv_echo_cb, NULL, wg);
    xylem_ws_conn_t*     c = _connect(l, NULL);
    xylem_ws_set_read_deadline(
        c,
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 40);
    char buf[16] = "unchanged";
    ASSERT(xylem_ws_recv(c, buf, sizeof(buf), NULL) == -1);
    ASSERT(!memcmp(buf, "unchanged", 10));
    xylem_ws_set_read_deadline(c, 0);
    ASSERT(xylem_ws_recv(c, buf, sizeof(buf), NULL) == -1);
    ASSERT(xylem_ws_send(c, "x", 1, XYLEM_WS_TEXT) == -1);
    ASSERT(xylem_ws_close(c, 1000, NULL, 0) == 0);
    _drain(c, l, wg);
}

static void test_write_timeout(void* arg) {
    (void)arg;
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l = _listen(_srv_echo_cb, NULL, wg);
    xylem_ws_conn_t*     c = _connect(l, NULL);
    xylem_ws_set_write_deadline(
        c,
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) - 1);
    ASSERT(xylem_ws_send(c, "x", 1, XYLEM_WS_TEXT) == -1);
    xylem_ws_set_write_deadline(c, 0);
    ASSERT(xylem_ws_ping(c, NULL, 0) == -1);
    _drain(c, l, wg);
}

static void _close_read_timer_cb(xylem_timer_t* timer, void* arg) {
    (void)timer;
    _close_read_t* ctx = arg;
    ASSERT(xylem_ws_close(ctx->conn, 1000, NULL, 0) == 0);
    xylem_waitgroup_done(ctx->wg);
}

static void test_close_with_blocked_receive(void* arg) {
    (void)arg;
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l      = _listen(_srv_echo_cb, NULL, wg);
    xylem_ws_conn_t*     c      = _connect(l, NULL);
    xylem_waitgroup_t*   closed = xylem_waitgroup_create();
    ASSERT(closed);
    _close_read_t ctx = {.conn = c, .wg = closed};
    xylem_waitgroup_add(closed, 1);
    xylem_timer_t* timer = xylem_timer_after(40, _close_read_timer_cb, &ctx);
    ASSERT(timer);
    char buf[16] = "unchanged";
    ASSERT(xylem_ws_recv(c, buf, sizeof(buf), NULL) == -1);
    xylem_waitgroup_wait(closed);
    xylem_timer_destroy(timer);
    xylem_waitgroup_destroy(closed);
    _drain(c, l, wg);
}

static void test_short_buffer_retry(void* arg) {
    (void)arg;
    const xylem_ws_opts_t options[] = {
        {0},
        {.fragment_threshold = 3},
        {.fragment_threshold       = 3,
         .permessage_deflate       = true,
         .deflate_context_takeover = true},
    };
    const char    payload[] = "retained payload";
    const uint8_t binary[]  = {0x00, 0xFF, 0x81, 0x03};
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        xylem_waitgroup_t* wg = xylem_waitgroup_create();
        ASSERT(wg);
        xylem_ws_listener_t* l = _listen(_srv_echo_cb, &options[i], wg);
        xylem_ws_conn_t*     c = _connect(l, &options[i]);
        ASSERT(
            xylem_ws_send(c, payload, sizeof(payload) - 1, XYLEM_WS_TEXT) == 0);
        ASSERT(xylem_ws_send(c, binary, sizeof(binary), XYLEM_WS_BINARY) == 0);
        char              small[4] = "keep";
        xylem_ws_opcode_t type     = XYLEM_WS_BINARY;
        ASSERT(xylem_ws_recv(c, NULL, 0, &type) == -1);
        ASSERT(type == XYLEM_WS_BINARY);
        for (int retry = 0; retry < 2; ++retry) {
            ASSERT(xylem_ws_recv(c, small, sizeof(small), &type) == -1);
            ASSERT(!memcmp(small, "keep", sizeof(small)));
            ASSERT(type == XYLEM_WS_BINARY);
        }
        char buf[sizeof(payload)];
        memset(buf, 0x5A, sizeof(buf));
        ASSERT(
            xylem_ws_recv(c, buf, sizeof(payload) - 1, &type) ==
            sizeof(payload) - 1);
        ASSERT(type == XYLEM_WS_TEXT);
        ASSERT(!memcmp(buf, payload, sizeof(payload) - 1));
        ASSERT(buf[sizeof(buf) - 1] == 0x5A);
        ASSERT(xylem_ws_recv(c, buf, sizeof(buf), &type) == sizeof(binary));
        ASSERT(type == XYLEM_WS_BINARY);
        ASSERT(!memcmp(buf, binary, sizeof(binary)));
        _echo_roundtrip(c, XYLEM_WS_TEXT, "after retry", 11);
        _drain(c, l, wg);
    }
}

static void test_empty_messages(void* arg) {
    (void)arg;
    const xylem_ws_opts_t options[] = {
        {0},
        {.permessage_deflate = true, .deflate_context_takeover = true},
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        xylem_waitgroup_t* wg = xylem_waitgroup_create();
        ASSERT(wg);
        xylem_ws_listener_t* l    = _listen(_srv_echo_cb, &options[i], wg);
        xylem_ws_conn_t*     c    = _connect(l, &options[i]);
        xylem_ws_opcode_t    type = XYLEM_WS_BINARY;
        ASSERT(xylem_ws_send(c, NULL, 0, XYLEM_WS_TEXT) == 0);
        ASSERT(xylem_ws_recv(c, NULL, 0, &type) == 0);
        ASSERT(type == XYLEM_WS_TEXT);
        ASSERT(xylem_ws_send(c, NULL, 0, XYLEM_WS_BINARY) == 0);
        ASSERT(xylem_ws_recv(c, NULL, 0, &type) == 0);
        ASSERT(type == XYLEM_WS_BINARY);
        ASSERT(xylem_ws_send(c, "next", 4, XYLEM_WS_TEXT) == 0);
        char buf[4];
        ASSERT(xylem_ws_recv(c, buf, sizeof(buf), NULL) == 4);
        ASSERT(!memcmp(buf, "next", sizeof(buf)));
        _drain(c, l, wg);
    }
}

static void test_invalid_buffer_arguments(void* arg) {
    (void)arg;
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l      = _listen(_srv_echo_cb, NULL, wg);
    xylem_ws_conn_t*     c      = _connect(l, NULL);
    char                 buf[4] = "keep";
    xylem_ws_opcode_t    type   = XYLEM_WS_BINARY;
    ASSERT(xylem_ws_send(c, NULL, 1, XYLEM_WS_TEXT) == -1);
    ASSERT(xylem_ws_send(c, buf, -1, XYLEM_WS_TEXT) == -1);
    ASSERT(xylem_ws_send(c, buf, sizeof(buf), (xylem_ws_opcode_t)2) == -1);
    ASSERT(xylem_ws_recv(c, NULL, 1, &type) == -1);
    ASSERT(xylem_ws_recv(c, buf, -1, &type) == -1);
    ASSERT(!memcmp(buf, "keep", sizeof(buf)));
    ASSERT(type == XYLEM_WS_BINARY);
    _echo_roundtrip(c, XYLEM_WS_TEXT, "valid", 5);
    _drain(c, l, wg);
}

static void test_close_with_pending_message(void* arg) {
    (void)arg;
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l = _listen(_srv_echo_cb, NULL, wg);
    xylem_ws_conn_t*     c = _connect(l, NULL);
    ASSERT(xylem_ws_send(c, "pending", 7, XYLEM_WS_TEXT) == 0);
    char byte = 'x';
    ASSERT(xylem_ws_recv(c, &byte, 1, NULL) == -1);
    ASSERT(byte == 'x');
    ASSERT(xylem_ws_close(c, 1000, NULL, 0) == 0);
    _drain(c, l, wg);
}

static void test_destroy_with_pending_message(void* arg) {
    (void)arg;
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_ws_listener_t* l = _listen(_srv_echo_cb, NULL, wg);
    xylem_ws_conn_t*     c = _connect(l, NULL);
    ASSERT(xylem_ws_send(c, "pending", 7, XYLEM_WS_TEXT) == 0);
    ASSERT(xylem_ws_recv(c, NULL, 0, NULL) == -1);
    xylem_ws_destroy(c);
    xylem_waitgroup_wait(wg);
    xylem_ws_destroy_listener(l);
    xylem_waitgroup_destroy(wg);
}


static void _test_run_all_cb(void* arg) {
    (void)arg;
    const _test_fn_t tests[] = {
        test_url_parsing,
        test_null_guards,
        test_short_buffer_retry,
        test_empty_messages,
        test_invalid_buffer_arguments,
        test_close_with_pending_message,
        test_destroy_with_pending_message,
        test_cancel_accept,
        test_cancel_handshake,
        test_addresses_and_deadline_clear,
        test_read_timeout,
        test_write_timeout,
        test_close_with_blocked_receive,
        test_bad_handshakes,
        test_handshake_accept_vector,
        test_client_handshake_request_variants,
        test_client_handshake_borrows_transport,
        test_server_handshake_stages,
        test_server_handshake_failure_ownership,
        test_client_handshake_parser,
        test_upgrade_with_first_frame,
        test_close_listener_with_connection,
        test_message_roundtrips,
        test_large_message,
        test_server_close,
        test_deflate_text_echo,
        test_deflate_context_takeover,
        test_deflate_large_binary,
        test_deflate_disabled_fallback,
    };
    const size_t test_count = sizeof(tests) / sizeof(tests[0]);
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    for (size_t i = 0; i < test_count; i++) {
        tests[i](NULL);
    }
    printf("All %zu WS tests passed.\n", test_count);
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all_cb, NULL, NULL);
    return 0;
}
