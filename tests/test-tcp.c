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

#include <string.h>

#define TCP_PORT          18080
#define SAFETY_TIMEOUT_MS 5000

typedef struct {
    xylem_loop_t*       loop;
    xylem_tcp_server_t* server;
    xylem_tcp_conn_t*   srv_conn;
    xylem_tcp_conn_t*   cli_conn;
    int                 close_called;
    int                 connect_called;
    int                 read_count;
    int                 verified;
    int                 wd_called;
    int                 wd_status;
    size_t              wd_len;
    int                 send_result;
    int                 tested;
    int                 value;
    int                 magic;
    char                received[128];
    size_t              received_len;
    char                frames[2][16];
    size_t              frame_lens[2];
} _test_ctx_t;

/* Custom frame parsers. */

static int _custom_parse_4(const void* data, size_t len) {
    (void)data;
    return (len >= 4) ? 4 : 0;
}

static int _custom_parse_zero(const void* data, size_t len) {
    (void)data;
    (void)len;
    return 0;
}

static int _custom_parse_neg(const void* data, size_t len) {
    (void)data;
    (void)len;
    return -1;
}

/* Shared callbacks. */

static void _safety_timeout_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer,
                                void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

static void _srv_accept_cb(xylem_tcp_server_t* server,
                            xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_server_get_userdata(server);
    xylem_tcp_set_userdata(conn, ctx);
}

static void _srv_close_cb(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->close_called = 1;
        xylem_loop_stop(ctx->loop);
    }
}

static void _srv_read_one_cb(xylem_tcp_conn_t* conn,
                              void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->read_count = 1;
    if (len <= sizeof(ctx->received)) {
        memcpy(ctx->received, data, len);
        ctx->received_len = len;
    }
    xylem_loop_stop(ctx->loop);
}

static void _srv_read_two_cb(xylem_tcp_conn_t* conn,
                              void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx->read_count < 2 && len <= sizeof(ctx->frames[0])) {
        memcpy(ctx->frames[ctx->read_count], data, len);
        ctx->frame_lens[ctx->read_count] = len;
    }
    ctx->read_count++;
    if (ctx->read_count >= 2) {
        xylem_loop_stop(ctx->loop);
    }
}

/* Variant of _srv_read_two_cb that asserts len==4 (for frame_fixed, custom_pos). */
static void _srv_read_two_fixed4_cb(xylem_tcp_conn_t* conn,
                                     void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx->read_count < 2) {
        ASSERT(len == 4);
        memcpy(ctx->frames[ctx->read_count], data, 4);
    }
    ctx->read_count++;
    if (ctx->read_count >= 2) {
        xylem_loop_stop(ctx->loop);
    }
}

/* Test-specific callbacks. */

static void _close_active_timer_cb(xylem_loop_t* loop,
                                    xylem_loop_timer_t* timer,
                                    void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_tcp_close_server(ctx->server);
    ctx->server = NULL;
}

static void _close_active_on_accept(xylem_tcp_server_t* server,
                                     xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tcp_server_get_userdata(server);
    ctx->srv_conn = conn;
    xylem_tcp_set_userdata(conn, ctx);
}

static void _close_active_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->close_called++;
    }
    xylem_loop_stop(ctx->loop);
}

static void _dial_on_connect(xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->connect_called = 1;
    xylem_loop_stop(ctx->loop);
}

static void _close_empty_on_connect(xylem_tcp_conn_t* conn) {
    xylem_tcp_close(conn);
}

static void _close_empty_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->close_called = 1;
        xylem_loop_stop(ctx->loop);
    }
}

static void _send_basic_on_connect(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "data", 4);
}

static void _send_basic_on_write_done(xylem_tcp_conn_t* conn,
                                       void* data, size_t len,
                                       int status) {
    (void)data;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->wd_called = 1;
    ctx->wd_status = status;
    ctx->wd_len    = len;
    xylem_loop_stop(ctx->loop);
}

static void _sac_on_connect(xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    xylem_tcp_send(conn, "pending", 7);
    xylem_tcp_close(conn);
    ctx->send_result = xylem_tcp_send(conn, "x", 1);
    ctx->tested = 1;
    xylem_loop_stop(ctx->loop);
}

static void _conn_ud_on_accept(xylem_tcp_server_t* server,
                                xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tcp_server_get_userdata(server);
    xylem_tcp_set_userdata(conn, &ctx->value);

    void* got = xylem_tcp_get_userdata(conn);
    ASSERT(got == &ctx->value);
    ASSERT(*(int*)got == 42);
    ctx->verified = 1;
    xylem_loop_stop(ctx->loop);
}

static void _srv_ud_on_accept(xylem_tcp_server_t* server,
                               xylem_tcp_conn_t* conn) {
    (void)conn;
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tcp_server_get_userdata(server);
    ASSERT(ctx != NULL);
    ASSERT(ctx->magic == 99);
    ctx->verified = 1;
    xylem_loop_stop(ctx->loop);
}

static void _peer_addr_on_accept(xylem_tcp_server_t* server,
                                  xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tcp_server_get_userdata(server);

    const xylem_addr_t* peer = xylem_tcp_get_peer_addr(conn);
    ASSERT(peer != NULL);

    const struct sockaddr* sa = (const struct sockaddr*)&peer->storage;
    ASSERT(sa->sa_family == AF_INET);

    ctx->verified = 1;
    xylem_loop_stop(ctx->loop);
}

static void _get_loop_on_connect(xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);

    xylem_loop_t* got = xylem_tcp_get_loop(conn);
    ASSERT(got == ctx->loop);

    ctx->verified = 1;
    xylem_loop_stop(ctx->loop);
}

static void _frame_none_srv_on_read(xylem_tcp_conn_t* conn,
                                     void* data, size_t len) {
    xylem_tcp_send(conn, data, len);
}

static void _frame_none_cli_on_connect(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "hello", 5);
}

static void _frame_none_cli_on_read(xylem_tcp_conn_t* conn,
                                     void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);

    if (ctx->received_len + len <= sizeof(ctx->received)) {
        memcpy(ctx->received + ctx->received_len, data, len);
        ctx->received_len += len;
    }

    if (ctx->received_len >= 5) {
        ctx->verified = 1;
        xylem_loop_stop(ctx->loop);
    }
}

static void _cli_send_8bytes_cb(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "ABCDEFGH", 8);
}

static void _cli_send_data_cb(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "data", 4);
}

static void _cli_send_len_be_cb(xylem_tcp_conn_t* conn) {
    uint8_t frame[7] = {0x00, 0x05, 'H', 'E', 'L', 'L', 'O'};
    xylem_tcp_send(conn, frame, sizeof(frame));
}

static void _cli_send_len_le_cb(xylem_tcp_conn_t* conn) {
    uint8_t frame[7] = {0x05, 0x00, 'H', 'E', 'L', 'L', 'O'};
    xylem_tcp_send(conn, frame, sizeof(frame));
}

static void _cli_send_varint_cb(xylem_tcp_conn_t* conn) {
    uint8_t frame[16];
    size_t pos = 0;
    xylem_varint_encode(5, frame, sizeof(frame), &pos);
    memcpy(frame + pos, "WORLD", 5);
    pos += 5;
    xylem_tcp_send(conn, frame, pos);
}

static void _cli_send_len_adj_cb(xylem_tcp_conn_t* conn) {
    /**
     * Length field value includes header size.
     * [0x00, 0x07, "HELLO"] -- length=7 means total frame=7,
     * so payload = 7 - 2 (header) = 5.
     * With adjustment=-2: frame_size = 2 + 7 + (-2) = 7, payload = 7 - 2 = 5.
     */
    uint8_t frame[7] = {0x00, 0x07, 'H', 'E', 'L', 'L', 'O'};
    xylem_tcp_send(conn, frame, sizeof(frame));
}

static void _cli_send_len_empty_cb(xylem_tcp_conn_t* conn) {
    /**
     * adjustment=-3, header_size=2, length field value=1, payload="X"
     * frame_size = 2 + 1 + (-3) = 0 which is <= 0, extraction returns -1.
     */
    uint8_t frame[3] = {0x00, 0x01, 'X'};
    xylem_tcp_send(conn, frame, sizeof(frame));
}

static void _cli_send_delim_multi_cb(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "hello\r\nworld\r\n", 14);
}

static void _cli_send_delim_single_cb(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "abc\ndef\n", 8);
}

/* Custom-zero on_read: counts reads but should never fire. */
static void _custom_zero_srv_on_read(xylem_tcp_conn_t* conn,
                                      void* data, size_t len) {
    (void)data;
    (void)len;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->read_count++;
}

/* Timer to stop the loop for custom_zero test. */
static void _custom_zero_check_cb(xylem_loop_t* loop,
                                   xylem_loop_timer_t* timer,
                                   void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

/* Test functions. */

static void test_listen_and_close(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t handler = {0};
    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &handler, NULL);
    ASSERT(server != NULL);

    xylem_tcp_close_server(server);

    /* Run briefly to let the close propagate. */
    xylem_loop_run(loop);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_close_server_with_active_conn(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t handler = {
        .on_accept = _close_active_on_accept,
        .on_close  = _close_active_on_close,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &handler, NULL);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    /* Timer fires after accept to close the server. */
    xylem_loop_timer_t* close_timer = xylem_loop_create_timer(loop, &ctx);
    xylem_loop_start_timer(close_timer, _close_active_timer_cb, 100, 0);

    xylem_tcp_handler_t cli_handler = {0};
    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called >= 1);

    xylem_loop_destroy_timer(close_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_close_server_idempotent(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t handler = {0};
    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &handler, NULL);
    ASSERT(server != NULL);

    /* Call close twice -- second call must not crash. */
    xylem_tcp_close_server(server);
    xylem_tcp_close_server(server);

    xylem_loop_run(loop);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_dial_connect(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {0};
    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _dial_on_connect,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.connect_called == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_close_empty_queue(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {0};
    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _close_empty_on_connect,
        .on_close   = _close_empty_on_close,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_send_basic(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {0};
    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect    = _send_basic_on_connect,
        .on_write_done = _send_basic_on_write_done,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.wd_called == 1);
    ASSERT(ctx.wd_status == 0);
    ASSERT(ctx.wd_len == 4);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_send_after_close(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {0};
    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _sac_on_connect,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.tested == 1);
    ASSERT(ctx.send_result == -1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_conn_userdata(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop  = loop;
    ctx.value = 42;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _conn_ud_on_accept,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);
    xylem_tcp_server_set_userdata(server, &ctx);

    xylem_tcp_handler_t cli_handler = {0};
    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.verified == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_server_userdata(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop  = loop;
    ctx.magic = 99;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_ud_on_accept,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);
    xylem_tcp_server_set_userdata(server, &ctx);

    /* Verify round-trip before any callback. */
    void* got = xylem_tcp_server_get_userdata(server);
    ASSERT(got == &ctx);
    ASSERT(((_test_ctx_t*)got)->magic == 99);

    xylem_tcp_handler_t cli_handler = {0};
    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.verified == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_peer_addr(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _peer_addr_on_accept,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);
    xylem_tcp_server_set_userdata(server, &ctx);

    xylem_tcp_handler_t cli_handler = {0};
    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.verified == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_get_loop(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {0};
    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _get_loop_on_connect,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.verified == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_none(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_read = _frame_none_srv_on_read,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, NULL);
    ASSERT(ctx.server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _frame_none_cli_on_connect,
        .on_read    = _frame_none_cli_on_read,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.verified == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_fixed(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type = XYLEM_TCP_FRAME_FIXED;
    srv_opts.framing.fixed.frame_size = 4;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_two_fixed4_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_8bytes_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.read_count == 2);
    ASSERT(memcmp(ctx.frames[0], "ABCD", 4) == 0);
    ASSERT(memcmp(ctx.frames[1], "EFGH", 4) == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_fixed_zero(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type = XYLEM_TCP_FRAME_FIXED;
    srv_opts.framing.fixed.frame_size = 0;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_close  = _srv_close_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_data_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_length_be(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type = XYLEM_TCP_FRAME_LENGTH;
    srv_opts.framing.length.header_size      = 2;
    srv_opts.framing.length.field_offset     = 0;
    srv_opts.framing.length.field_size       = 2;
    srv_opts.framing.length.coding           = XYLEM_TCP_LENGTH_FIXEDINT;
    srv_opts.framing.length.field_big_endian = true;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_one_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_len_be_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.read_count == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "HELLO", 5) == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_length_le(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type = XYLEM_TCP_FRAME_LENGTH;
    srv_opts.framing.length.header_size      = 2;
    srv_opts.framing.length.field_offset     = 0;
    srv_opts.framing.length.field_size       = 2;
    srv_opts.framing.length.coding           = XYLEM_TCP_LENGTH_FIXEDINT;
    srv_opts.framing.length.field_big_endian = false;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_one_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_len_le_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.read_count == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "HELLO", 5) == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_length_field_size_zero(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type = XYLEM_TCP_FRAME_LENGTH;
    srv_opts.framing.length.header_size      = 2;
    srv_opts.framing.length.field_offset     = 0;
    srv_opts.framing.length.field_size       = 0;
    srv_opts.framing.length.coding           = XYLEM_TCP_LENGTH_FIXEDINT;
    srv_opts.framing.length.field_big_endian = true;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_close  = _srv_close_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_len_be_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_length_field_size_over8(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type = XYLEM_TCP_FRAME_LENGTH;
    srv_opts.framing.length.header_size      = 2;
    srv_opts.framing.length.field_offset     = 0;
    srv_opts.framing.length.field_size       = 9;
    srv_opts.framing.length.coding           = XYLEM_TCP_LENGTH_FIXEDINT;
    srv_opts.framing.length.field_big_endian = true;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_close  = _srv_close_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_len_be_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_length_varint(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type = XYLEM_TCP_FRAME_LENGTH;
    srv_opts.framing.length.header_size  = 1;
    srv_opts.framing.length.field_offset = 0;
    srv_opts.framing.length.field_size   = 1;
    srv_opts.framing.length.coding       = XYLEM_TCP_LENGTH_VARINT;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_one_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_varint_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.read_count == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "WORLD", 5) == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_length_adjustment(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type = XYLEM_TCP_FRAME_LENGTH;
    srv_opts.framing.length.header_size      = 2;
    srv_opts.framing.length.field_offset     = 0;
    srv_opts.framing.length.field_size       = 2;
    srv_opts.framing.length.coding           = XYLEM_TCP_LENGTH_FIXEDINT;
    srv_opts.framing.length.field_big_endian = true;
    srv_opts.framing.length.adjustment       = -2;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_one_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_len_adj_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.read_count == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "HELLO", 5) == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_length_empty_payload(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type = XYLEM_TCP_FRAME_LENGTH;
    srv_opts.framing.length.header_size      = 2;
    srv_opts.framing.length.field_offset     = 0;
    srv_opts.framing.length.field_size       = 2;
    srv_opts.framing.length.coding           = XYLEM_TCP_LENGTH_FIXEDINT;
    srv_opts.framing.length.field_big_endian = true;
    srv_opts.framing.length.adjustment       = -3;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_close  = _srv_close_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_len_empty_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_delim_multi(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type            = XYLEM_TCP_FRAME_DELIM;
    srv_opts.framing.delim.delim     = "\r\n";
    srv_opts.framing.delim.delim_len = 2;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_two_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_delim_multi_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.read_count == 2);
    ASSERT(ctx.frame_lens[0] == 5);
    ASSERT(memcmp(ctx.frames[0], "hello", 5) == 0);
    ASSERT(ctx.frame_lens[1] == 5);
    ASSERT(memcmp(ctx.frames[1], "world", 5) == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_delim_single(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type            = XYLEM_TCP_FRAME_DELIM;
    srv_opts.framing.delim.delim     = "\n";
    srv_opts.framing.delim.delim_len = 1;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_two_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_delim_single_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.read_count == 2);
    ASSERT(ctx.frame_lens[0] == 3);
    ASSERT(memcmp(ctx.frames[0], "abc", 3) == 0);
    ASSERT(ctx.frame_lens[1] == 3);
    ASSERT(memcmp(ctx.frames[1], "def", 3) == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_delim_null(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type            = XYLEM_TCP_FRAME_DELIM;
    srv_opts.framing.delim.delim     = NULL;
    srv_opts.framing.delim.delim_len = 0;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_close  = _srv_close_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_data_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_custom_positive(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type         = XYLEM_TCP_FRAME_CUSTOM;
    srv_opts.framing.custom.parse = _custom_parse_4;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_two_fixed4_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_8bytes_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.read_count == 2);
    ASSERT(memcmp(ctx.frames[0], "ABCD", 4) == 0);
    ASSERT(memcmp(ctx.frames[1], "EFGH", 4) == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_custom_zero(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type         = XYLEM_TCP_FRAME_CUSTOM;
    srv_opts.framing.custom.parse = _custom_parse_zero;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_read   = _custom_zero_srv_on_read,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    /* Timer to stop the loop after 200ms. */
    xylem_loop_timer_t* check = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(check, _custom_zero_check_cb, 200, 0);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_data_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.read_count == 0);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(check);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_custom_negative(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type         = XYLEM_TCP_FRAME_CUSTOM;
    srv_opts.framing.custom.parse = _custom_parse_neg;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_close  = _srv_close_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_data_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void test_frame_custom_null_parse(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type         = XYLEM_TCP_FRAME_CUSTOM;
    srv_opts.framing.custom.parse = NULL;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_close  = _srv_close_cb,
    };

    ctx.server = xylem_tcp_listen(loop, &addr, &srv_handler, &srv_opts);
    ASSERT(ctx.server != NULL);
    xylem_tcp_server_set_userdata(ctx.server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _cli_send_data_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _read_timeout_cb(xylem_tcp_conn_t* conn,
                              xylem_tcp_timeout_type_t type) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (type == XYLEM_TCP_TIMEOUT_READ) {
        ctx->verified = 1;
    }
    xylem_tcp_close(conn);
    xylem_loop_stop(ctx->loop);
}

static void _read_timeout_accept_cb(xylem_tcp_server_t* server,
                                     xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_server_get_userdata(server);
    xylem_tcp_set_userdata(conn, ctx);
}

static void test_read_timeout(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.read_timeout_ms = 100;

    xylem_tcp_handler_t srv_handler = {
        .on_accept  = _read_timeout_accept_cb,
        .on_timeout = _read_timeout_cb,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, &srv_opts);
    ASSERT(server != NULL);
    xylem_tcp_server_set_userdata(server, &ctx);

    /* Client connects but sends nothing. */
    xylem_tcp_handler_t cli_handler = {0};
    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.verified == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _write_timeout_cli_connect_cb(xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    /**
     * Flood the write queue to saturate the kernel send buffer.
     * The server never reads, so TCP flow control will eventually
     * stall the sender and the write timeout should fire.
     */
    char buf[65536];
    memset(buf, 'X', sizeof(buf));
    for (int i = 0; i < 2048; i++) {
        if (xylem_tcp_send(conn, buf, sizeof(buf)) != 0) {
            break;
        }
    }
    ctx->connect_called = 1;
}

static void _write_timeout_cb(xylem_tcp_conn_t* conn,
                               xylem_tcp_timeout_type_t type) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (type == XYLEM_TCP_TIMEOUT_WRITE) {
        ctx->verified = 1;
    }
    xylem_tcp_close(conn);
    xylem_loop_stop(ctx->loop);
}

static void test_write_timeout(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, 10000, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    /* Server accepts but never reads -- no on_read handler. */
    xylem_tcp_handler_t srv_handler = {0};
    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);

    xylem_tcp_opts_t cli_opts = {0};
    cli_opts.write_timeout_ms = 1;

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _write_timeout_cli_connect_cb,
        .on_timeout = _write_timeout_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, &cli_opts);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.verified == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _connect_timeout_cb(xylem_tcp_conn_t* conn,
                                 xylem_tcp_timeout_type_t type) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (type == XYLEM_TCP_TIMEOUT_CONNECT) {
        ctx->verified = 1;
    }
    xylem_tcp_close(conn);
    xylem_loop_stop(ctx->loop);
}

static void _connect_timeout_close_cb(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        /**
         * On some platforms the connect fails immediately with
         * EHOSTUNREACH instead of timing out. Accept either path
         * as valid -- the important thing is the connection does
         * not succeed.
         */
        if (!ctx->verified) {
            ctx->verified = 1;
        }
        xylem_loop_stop(ctx->loop);
    }
}

static void test_connect_timeout(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    /* RFC 5737 TEST-NET -- unreachable, triggers connect timeout. */
    xylem_addr_t addr;
    xylem_addr_pton("192.0.2.1", TCP_PORT, &addr);

    xylem_tcp_opts_t cli_opts = {0};
    cli_opts.connect_timeout_ms = 200;

    xylem_tcp_handler_t cli_handler = {
        .on_timeout = _connect_timeout_cb,
        .on_close   = _connect_timeout_close_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, &cli_opts);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.verified == 1);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _heartbeat_miss_cb(xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->verified = 1;
    xylem_tcp_close(conn);
    xylem_loop_stop(ctx->loop);
}

static void test_heartbeat_miss(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.heartbeat_ms = 100;

    xylem_tcp_handler_t srv_handler = {
        .on_accept         = _srv_accept_cb,
        .on_heartbeat_miss = _heartbeat_miss_cb,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, &srv_opts);
    ASSERT(server != NULL);
    xylem_tcp_server_set_userdata(server, &ctx);

    /* Client connects but sends nothing. */
    xylem_tcp_handler_t cli_handler = {0};
    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.verified == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _hb_reset_send_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer,
                                void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    if (ctx->cli_conn) {
        xylem_tcp_send(ctx->cli_conn, "x", 1);
    }
}

static void _hb_reset_stop_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer,
                                void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

static void _hb_reset_miss_cb(xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    /* Should NOT fire -- mark it so we can detect failure. */
    ctx->verified = 1;
}

static void _hb_reset_cli_connect_cb(xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->cli_conn = conn;
}

static void test_heartbeat_reset_on_data(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.heartbeat_ms = 200;

    xylem_tcp_handler_t srv_handler = {
        .on_accept         = _srv_accept_cb,
        .on_heartbeat_miss = _hb_reset_miss_cb,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, &srv_opts);
    ASSERT(server != NULL);
    xylem_tcp_server_set_userdata(server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _hb_reset_cli_connect_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    /* Send data every 100ms to keep heartbeat alive. */
    xylem_loop_timer_t* send_timer = xylem_loop_create_timer(loop, &ctx);
    xylem_loop_start_timer(send_timer, _hb_reset_send_cb, 100, 100);

    /* Stop after 500ms. */
    xylem_loop_timer_t* stop_timer = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(stop_timer, _hb_reset_stop_cb, 500, 0);

    xylem_loop_run(loop);

    /* verified==0 means heartbeat_miss was NOT called. */
    ASSERT(ctx.verified == 0);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(send_timer);
    xylem_loop_destroy_timer(stop_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _reconnect_delayed_listen_cb(xylem_loop_t* loop,
                                          xylem_loop_timer_t* timer,
                                          void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {0};
    ctx->server = xylem_tcp_listen(ctx->loop, &addr, &srv_handler, NULL);
}

static void _reconnect_on_connect_cb(xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->connect_called = 1;
    xylem_loop_stop(ctx->loop);
}

static void test_reconnect_success(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    /* Start server after 600ms so initial connect fails. */
    xylem_loop_timer_t* delay = xylem_loop_create_timer(loop, &ctx);
    xylem_loop_start_timer(delay, _reconnect_delayed_listen_cb, 600, 0);

    xylem_tcp_opts_t cli_opts = {0};
    cli_opts.reconnect_max = 3;

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _reconnect_on_connect_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, &cli_opts);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.connect_called == 1);

    if (ctx.server) {
        xylem_tcp_close_server(ctx.server);
    }
    xylem_loop_destroy_timer(delay);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _reconnect_limit_close_cb(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->close_called = 1;
        xylem_loop_stop(ctx->loop);
    }
}

static void test_reconnect_limit(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    /* Port 18081 -- no server listening. */
    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT + 1, &addr);

    xylem_tcp_opts_t cli_opts = {0};
    cli_opts.reconnect_max = 1;
    cli_opts.connect_timeout_ms = 1000;

    xylem_tcp_handler_t cli_handler = {
        .on_close = _reconnect_limit_close_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, &cli_opts);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _read_buf_full_srv_close_cb(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->close_called = 1;
        xylem_loop_stop(ctx->loop);
    }
}

static void _read_buf_full_cli_connect_cb(xylem_tcp_conn_t* conn) {
    char buf[64];
    memset(buf, 'A', sizeof(buf));
    xylem_tcp_send(conn, buf, sizeof(buf));
}

static void test_read_buf_full(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    /**
     * Use FRAME_FIXED with frame_size > read_buf_size so the buffer
     * fills before a complete frame can be extracted.
     */
    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.read_buf_size            = 8;
    srv_opts.framing.type             = XYLEM_TCP_FRAME_FIXED;
    srv_opts.framing.fixed.frame_size = 16;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_close  = _read_buf_full_srv_close_cb,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, &srv_opts);
    ASSERT(server != NULL);
    xylem_tcp_server_set_userdata(server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _read_buf_full_cli_connect_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _peer_eof_srv_close_cb(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->close_called = 1;
        xylem_loop_stop(ctx->loop);
    }
}

static void _peer_eof_cli_connect_cb(xylem_tcp_conn_t* conn) {
    xylem_tcp_close(conn);
}

static void test_peer_close_eof(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _srv_accept_cb,
        .on_close  = _peer_eof_srv_close_cb,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);
    xylem_tcp_server_set_userdata(server, &ctx);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _peer_eof_cli_connect_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _pending_writes_wd_cb(xylem_tcp_conn_t* conn,
                                   void* data, size_t len,
                                   int status) {
    (void)data;
    (void)len;
    (void)status;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->wd_called++;
}

static void _pending_writes_close_cb(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->close_called = 1;
        xylem_loop_stop(ctx->loop);
    }
}

static void _pending_writes_connect_cb(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "aaa", 3);
    xylem_tcp_send(conn, "bbb", 3);
    xylem_tcp_send(conn, "ccc", 3);
    xylem_tcp_close(conn);
}

static void test_close_pending_writes(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {0};
    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect    = _pending_writes_connect_cb,
        .on_write_done = _pending_writes_wd_cb,
        .on_close      = _pending_writes_close_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.wd_called == 3);
    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _drain_err_srv_accept_cb(xylem_tcp_server_t* server,
                                      xylem_tcp_conn_t* conn) {
    /* Server immediately closes its conn to simulate error. */
    (void)server;
    xylem_tcp_close(conn);
}

static void _drain_err_cli_connect_cb(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "data", 4);
}

static void _drain_err_cli_close_cb(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->close_called = 1;
        xylem_loop_stop(ctx->loop);
    }
}

static void test_drain_write_queue_on_error(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _drain_err_srv_accept_cb,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, NULL);
    ASSERT(server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _drain_err_cli_connect_cb,
        .on_close   = _drain_err_cli_close_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, NULL);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    ASSERT(ctx.close_called == 1);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


static void _life_srv_accept_cb(xylem_tcp_server_t* server,
                                 xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_server_get_userdata(server);
    ctx->srv_conn = conn;
    ctx->verified |= 1; /* bit 0: accept fired */
    xylem_tcp_set_userdata(conn, ctx);
}

static void _life_srv_read_cb(xylem_tcp_conn_t* conn,
                               void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->verified |= 2; /* bit 1: server read fired */

    /* Echo back "pong\r\n". */
    (void)data;
    (void)len;
    xylem_tcp_send(conn, "pong\r\n", 6);
}

static void _life_srv_close_cb(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->verified |= 4; /* bit 2: server close fired */
    }
}

static void _life_cli_connect_cb(xylem_tcp_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    ctx->verified |= 8; /* bit 3: client connect fired */
    xylem_tcp_send(conn, "ping\r\n", 6);
}

static void _life_cli_read_cb(xylem_tcp_conn_t* conn,
                               void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (len >= 4 && memcmp(data, "pong", 4) == 0) {
        ctx->verified |= 16; /* bit 4: client read fired */
    }
    /* Close both sides. */
    xylem_tcp_close(conn);
    if (ctx->srv_conn) {
        xylem_tcp_close(ctx->srv_conn);
    }
}

static void _life_cli_close_cb(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tcp_get_userdata(conn);
    if (ctx) {
        ctx->verified |= 32; /* bit 5: client close fired */
        xylem_loop_stop(ctx->loop);
    }
}

static void test_lifecycle_full(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(safety, _safety_timeout_cb, SAFETY_TIMEOUT_MS, 0);

    _test_ctx_t ctx = {0};
    ctx.loop = loop;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_opts_t srv_opts = {0};
    srv_opts.framing.type            = XYLEM_TCP_FRAME_DELIM;
    srv_opts.framing.delim.delim     = "\r\n";
    srv_opts.framing.delim.delim_len = 2;

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _life_srv_accept_cb,
        .on_read   = _life_srv_read_cb,
        .on_close  = _life_srv_close_cb,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr,
                                                   &srv_handler, &srv_opts);
    ASSERT(server != NULL);
    xylem_tcp_server_set_userdata(server, &ctx);

    xylem_tcp_opts_t cli_opts = {0};
    cli_opts.framing.type            = XYLEM_TCP_FRAME_DELIM;
    cli_opts.framing.delim.delim     = "\r\n";
    cli_opts.framing.delim.delim_len = 2;

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _life_cli_connect_cb,
        .on_read    = _life_cli_read_cb,
        .on_close   = _life_cli_close_cb,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(loop, &addr,
                                            &cli_handler, &cli_opts);
    ASSERT(cli != NULL);
    xylem_tcp_set_userdata(cli, &ctx);

    xylem_loop_run(loop);

    /* All 6 events must have fired: accept, srv_read, srv_close,
       cli_connect, cli_read, cli_close => 0x3F = 63. */
    ASSERT((ctx.verified & 0x3F) == 0x3F);

    xylem_tcp_close_server(server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(loop);
}


int main(void) {
    xylem_startup();

    test_listen_and_close();
    test_close_server_with_active_conn();
    test_close_server_idempotent();
    test_dial_connect();
    test_close_empty_queue();
    test_send_basic();
    test_send_after_close();
    test_conn_userdata();
    test_server_userdata();
    test_peer_addr();
    test_get_loop();
    test_frame_none();
    test_frame_fixed();
    test_frame_fixed_zero();
    test_frame_length_be();
    test_frame_length_le();
    test_frame_length_field_size_zero();
    test_frame_length_field_size_over8();
    test_frame_length_varint();
    test_frame_length_adjustment();
    test_frame_length_empty_payload();
    test_frame_delim_multi();
    test_frame_delim_single();
    test_frame_delim_null();
    test_frame_custom_positive();
    test_frame_custom_zero();
    test_frame_custom_negative();
    test_frame_custom_null_parse();
    test_read_timeout();
    test_write_timeout();
    test_connect_timeout();
    test_heartbeat_miss();
    test_heartbeat_reset_on_data();
    test_reconnect_success();
    test_reconnect_limit();
    test_read_buf_full();
    test_peer_close_eof();
    test_close_pending_writes();
    test_drain_write_queue_on_error();
    test_lifecycle_full();

    xylem_cleanup();
    return 0;
}
