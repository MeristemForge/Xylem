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

#include "xylem/xylem-tcp.h"
#include "assert.h"
#include <string.h>

#define TCP_PORT 18080
#define T(fn) { #fn, fn }

typedef void (*test_fn)(void);

typedef struct {
    const char* name;
    test_fn     fn;
} test_entry;

static xylem_loop_timer_t _safety_timer;

/* Test 1: TCP echo with FRAME_DELIM */
static xylem_tcp_conn_t*   _echo_server_conn = NULL;
static xylem_tcp_conn_t*   _echo_client_conn = NULL;
static xylem_tcp_server_t* _echo_server      = NULL;
static xylem_loop_t        _echo_loop;
static int    _echo_accept_called  = 0;
static int    _echo_connect_called = 0;
static int    _echo_read_called    = 0;
static char   _echo_received[64];
static size_t _echo_received_len = 0;

/* Test 2: TCP lifecycle */
static xylem_loop_t        _life_loop;
static xylem_tcp_server_t* _life_server   = NULL;
static xylem_tcp_conn_t*   _life_srv_conn = NULL;
static int _life_cli_connect = 0;
static int _life_cli_close   = 0;
static int _life_srv_accept  = 0;
static int _life_srv_close   = 0;
static xylem_loop_timer_t  _life_check_timer;

/* Test 3: TCP write done */
static xylem_loop_t        _wd_loop;
static xylem_tcp_server_t* _wd_server   = NULL;
static xylem_tcp_conn_t*   _wd_srv_conn = NULL;
static xylem_tcp_conn_t*   _wd_cli_conn = NULL;
static int    _wd_called  = 0;
static int    _wd_status  = -1;
static size_t _wd_len     = 0;

/* Test 4: TCP FRAME_FIXED */
static xylem_loop_t        _fix_loop;
static xylem_tcp_server_t* _fix_server   = NULL;
static xylem_tcp_conn_t*   _fix_srv_conn = NULL;
static xylem_tcp_conn_t*   _fix_cli_conn = NULL;
static int  _fix_read_count = 0;
static char _fix_frames[2][4];

/* Test 5: TCP userdata */
static xylem_loop_t        _ud_loop;
static xylem_tcp_server_t* _ud_server   = NULL;
static xylem_tcp_conn_t*   _ud_srv_conn = NULL;
static xylem_tcp_conn_t*   _ud_cli_conn = NULL;
static int _ud_value    = 42;
static int _ud_verified = 0;

/* Test 6: TCP send after close */
static xylem_loop_t        _sac_loop;
static xylem_tcp_server_t* _sac_server      = NULL;
static int                 _sac_send_result = 0;
static int                 _sac_tested      = 0;

static void _safety_timer_cb(xylem_loop_t* loop,
                              xylem_loop_timer_t* timer) {
    (void)timer;
    fprintf(stderr, "SAFETY TIMEOUT: test hung, stopping loop\n");
    xylem_loop_stop(loop);
}

static void _start_safety_timer(xylem_loop_t* loop) {
    xylem_loop_timer_init(loop, &_safety_timer);
    xylem_loop_timer_start(&_safety_timer, _safety_timer_cb, 2000, 0);
}

static void _stop_safety_timer(void) {
    xylem_loop_timer_close(&_safety_timer);
}

static void _echo_srv_on_accept(xylem_tcp_conn_t* conn) {
    _echo_server_conn = conn;
    _echo_accept_called = 1;
}

static void _echo_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == _echo_server_conn) _echo_server_conn = NULL;
}

static void _echo_srv_on_read(xylem_tcp_conn_t* conn,
                               void* data, size_t len) {
    char buf[128];
    if (len + 2 <= sizeof(buf)) {
        memcpy(buf, data, len);
        buf[len]     = '\r';
        buf[len + 1] = '\n';
        xylem_tcp_send(conn, buf, len + 2);
    }
}

static void _echo_cli_on_connect(xylem_tcp_conn_t* conn) {
    _echo_connect_called = 1;
    xylem_tcp_send(conn, "hello\r\n", 7);
}

static void _echo_cli_on_read(xylem_tcp_conn_t* conn,
                               void* data, size_t len) {
    (void)conn;
    _echo_read_called = 1;
    if (len < sizeof(_echo_received)) {
        memcpy(_echo_received, data, len);
        _echo_received_len = len;
    }
    xylem_loop_stop(&_echo_loop);
}

static void _echo_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == _echo_client_conn) _echo_client_conn = NULL;
}

static void test_tcp_echo_delim(void) {
    xylem_loop_init(&_echo_loop);
    _start_safety_timer(&_echo_loop);

    _echo_accept_called  = 0;
    _echo_connect_called = 0;
    _echo_read_called    = 0;
    _echo_received_len   = 0;
    _echo_server_conn    = NULL;
    _echo_client_conn    = NULL;
    _echo_server         = NULL;
    memset(_echo_received, 0, sizeof(_echo_received));

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _echo_srv_on_accept,
        .on_read   = _echo_srv_on_read,
        .on_close  = _echo_srv_on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type          = XYLEM_TCP_FRAME_DELIM;
    opts.framing.delim.delim     = "\r\n";
    opts.framing.delim.delim_len = 2;

    _echo_server = xylem_tcp_listen(&_echo_loop, &addr,
                                     &srv_handler, &opts);
    ASSERT(_echo_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _echo_cli_on_connect,
        .on_read    = _echo_cli_on_read,
        .on_close   = _echo_cli_on_close,
    };

    _echo_client_conn = xylem_tcp_dial(&_echo_loop, &addr,
                                        &cli_handler, &opts);
    ASSERT(_echo_client_conn != NULL);

    xylem_loop_run(&_echo_loop);

    ASSERT(_echo_accept_called == 1);
    ASSERT(_echo_connect_called == 1);
    ASSERT(_echo_read_called == 1);
    ASSERT(_echo_received_len == 5);
    ASSERT(memcmp(_echo_received, "hello", 5) == 0);

    _stop_safety_timer();
    if (_echo_server) { xylem_tcp_server_close(_echo_server); _echo_server = NULL; }
    xylem_loop_deinit(&_echo_loop);
}

static void _life_srv_on_accept(xylem_tcp_conn_t* conn) {
    _life_srv_accept = 1;
    _life_srv_conn = conn;
}

static void _life_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)conn; (void)err;
    _life_srv_close = 1;
}

static void _life_cli_on_connect(xylem_tcp_conn_t* conn) {
    _life_cli_connect = 1;
    xylem_tcp_close(conn);
}

static void _life_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)conn; (void)err;
    _life_cli_close = 1;
}

static void _life_check_cb(xylem_loop_t* loop,
                            xylem_loop_timer_t* timer) {
    (void)timer;
    if (_life_srv_conn && !_life_srv_close) {
        xylem_tcp_close(_life_srv_conn);
    }
    xylem_loop_stop(loop);
}

static void test_tcp_lifecycle(void) {
    xylem_loop_init(&_life_loop);
    _start_safety_timer(&_life_loop);

    _life_cli_connect = 0;
    _life_cli_close   = 0;
    _life_srv_accept  = 0;
    _life_srv_close   = 0;
    _life_srv_conn    = NULL;
    _life_server      = NULL;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _life_srv_on_accept,
        .on_close  = _life_srv_on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type = XYLEM_TCP_FRAME_NONE;

    _life_server = xylem_tcp_listen(&_life_loop, &addr,
                                     &srv_handler, &opts);
    ASSERT(_life_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _life_cli_on_connect,
        .on_close   = _life_cli_on_close,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&_life_loop, &addr,
                                           &cli_handler, &opts);
    ASSERT(cli != NULL);

    xylem_loop_timer_init(&_life_loop, &_life_check_timer);
    xylem_loop_timer_start(&_life_check_timer, _life_check_cb, 200, 0);

    xylem_loop_run(&_life_loop);

    ASSERT(_life_cli_connect == 1);
    ASSERT(_life_cli_close == 1);
    ASSERT(_life_srv_accept == 1);

    _stop_safety_timer();
    xylem_loop_timer_close(&_life_check_timer);
    if (_life_server) { xylem_tcp_server_close(_life_server); _life_server = NULL; }
    xylem_loop_deinit(&_life_loop);
}

static void _wd_srv_on_accept(xylem_tcp_conn_t* conn) {
    _wd_srv_conn = conn;
}

static void _wd_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == _wd_srv_conn) _wd_srv_conn = NULL;
}

static void _wd_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == _wd_cli_conn) _wd_cli_conn = NULL;
}

static void _wd_cli_on_write_done(xylem_tcp_conn_t* conn,
                                   void* data, size_t len, int status) {
    (void)data; (void)conn;
    _wd_called = 1;
    _wd_status = status;
    _wd_len    = len;
    xylem_loop_stop(&_wd_loop);
}

static void _wd_cli_on_connect(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "data", 4);
}

static void test_tcp_write_done(void) {
    xylem_loop_init(&_wd_loop);
    _start_safety_timer(&_wd_loop);

    _wd_called   = 0;
    _wd_status   = -1;
    _wd_len      = 0;
    _wd_srv_conn = NULL;
    _wd_cli_conn = NULL;
    _wd_server   = NULL;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _wd_srv_on_accept,
        .on_close  = _wd_srv_on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type = XYLEM_TCP_FRAME_NONE;

    _wd_server = xylem_tcp_listen(&_wd_loop, &addr,
                                   &srv_handler, &opts);
    ASSERT(_wd_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect    = _wd_cli_on_connect,
        .on_write_done = _wd_cli_on_write_done,
        .on_close      = _wd_cli_on_close,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&_wd_loop, &addr,
                                           &cli_handler, &opts);
    ASSERT(cli != NULL);
    _wd_cli_conn = cli;

    xylem_loop_run(&_wd_loop);

    ASSERT(_wd_called == 1);
    ASSERT(_wd_status == 0);
    ASSERT(_wd_len == 4);

    _stop_safety_timer();
    if (_wd_server) { xylem_tcp_server_close(_wd_server); _wd_server = NULL; }
    xylem_loop_deinit(&_wd_loop);
}

static void _fix_srv_on_accept(xylem_tcp_conn_t* conn) {
    _fix_srv_conn = conn;
}

static void _fix_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == _fix_srv_conn) _fix_srv_conn = NULL;
}

static void _fix_srv_on_read(xylem_tcp_conn_t* conn,
                              void* data, size_t len) {
    (void)conn;
    if (_fix_read_count < 2) {
        ASSERT(len == 4);
        memcpy(_fix_frames[_fix_read_count], data, 4);
    }
    _fix_read_count++;
    if (_fix_read_count >= 2) {
        xylem_loop_stop(&_fix_loop);
    }
}

static void _fix_cli_on_connect(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "ABCDEFGH", 8);
}

static void _fix_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == _fix_cli_conn) _fix_cli_conn = NULL;
}

static void test_tcp_frame_fixed(void) {
    xylem_loop_init(&_fix_loop);
    _start_safety_timer(&_fix_loop);

    _fix_read_count = 0;
    _fix_srv_conn   = NULL;
    _fix_cli_conn   = NULL;
    _fix_server     = NULL;
    memset(_fix_frames, 0, sizeof(_fix_frames));

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _fix_srv_on_accept,
        .on_read   = _fix_srv_on_read,
        .on_close  = _fix_srv_on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type            = XYLEM_TCP_FRAME_FIXED;
    opts.framing.fixed.frame_size = 4;

    _fix_server = xylem_tcp_listen(&_fix_loop, &addr,
                                    &srv_handler, &opts);
    ASSERT(_fix_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _fix_cli_on_connect,
        .on_close   = _fix_cli_on_close,
    };

    xylem_tcp_opts_t cli_opts = {0};
    cli_opts.framing.type = XYLEM_TCP_FRAME_NONE;

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&_fix_loop, &addr,
                                           &cli_handler, &cli_opts);
    ASSERT(cli != NULL);
    _fix_cli_conn = cli;

    xylem_loop_run(&_fix_loop);

    ASSERT(_fix_read_count == 2);
    ASSERT(memcmp(_fix_frames[0], "ABCD", 4) == 0);
    ASSERT(memcmp(_fix_frames[1], "EFGH", 4) == 0);

    _stop_safety_timer();
    if (_fix_server) { xylem_tcp_server_close(_fix_server); _fix_server = NULL; }
    xylem_loop_deinit(&_fix_loop);
}

static void _ud_srv_on_accept(xylem_tcp_conn_t* conn) {
    _ud_srv_conn = conn;
    xylem_tcp_conn_set_userdata(conn, &_ud_value);
    void* got = xylem_tcp_conn_get_userdata(conn);
    ASSERT(got == &_ud_value);
    ASSERT(*(int*)got == 42);
    _ud_verified = 1;
    xylem_loop_stop(&_ud_loop);
}

static void _ud_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == _ud_srv_conn) _ud_srv_conn = NULL;
}

static void _ud_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == _ud_cli_conn) _ud_cli_conn = NULL;
}

static void test_tcp_userdata(void) {
    xylem_loop_init(&_ud_loop);
    _start_safety_timer(&_ud_loop);

    _ud_verified  = 0;
    _ud_server    = NULL;
    _ud_srv_conn  = NULL;
    _ud_cli_conn  = NULL;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _ud_srv_on_accept,
        .on_close  = _ud_srv_on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type = XYLEM_TCP_FRAME_NONE;

    _ud_server = xylem_tcp_listen(&_ud_loop, &addr,
                                   &srv_handler, &opts);
    ASSERT(_ud_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_close = _ud_cli_on_close,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&_ud_loop, &addr,
                                           &cli_handler, &opts);
    ASSERT(cli != NULL);
    _ud_cli_conn = cli;

    xylem_loop_run(&_ud_loop);

    ASSERT(_ud_verified == 1);

    _stop_safety_timer();
    if (_ud_server) { xylem_tcp_server_close(_ud_server); _ud_server = NULL; }
    xylem_loop_deinit(&_ud_loop);
}

static void _sac_srv_on_accept(xylem_tcp_conn_t* conn) {
    (void)conn;
}

static void _sac_cli_on_connect(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "pending", 7);
    xylem_tcp_close(conn);
    _sac_send_result = xylem_tcp_send(conn, "x", 1);
    _sac_tested = 1;
    xylem_loop_stop(&_sac_loop);
}

static void test_tcp_send_after_close(void) {
    xylem_loop_init(&_sac_loop);
    _start_safety_timer(&_sac_loop);

    _sac_send_result = 0;
    _sac_tested      = 0;
    _sac_server      = NULL;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _sac_srv_on_accept,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type = XYLEM_TCP_FRAME_NONE;

    _sac_server = xylem_tcp_listen(&_sac_loop, &addr,
                                    &srv_handler, &opts);
    ASSERT(_sac_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _sac_cli_on_connect,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&_sac_loop, &addr,
                                           &cli_handler, &opts);
    ASSERT(cli != NULL);

    xylem_loop_run(&_sac_loop);

    ASSERT(_sac_tested == 1);
    ASSERT(_sac_send_result == -1);

    _stop_safety_timer();
    if (_sac_server) { xylem_tcp_server_close(_sac_server); _sac_server = NULL; }
    xylem_loop_deinit(&_sac_loop);
}

int main(void) {
    platform_socket_startup();

    test_entry tests[] = {
        T(test_tcp_echo_delim),
        T(test_tcp_lifecycle),
        T(test_tcp_write_done),
        T(test_tcp_frame_fixed),
        T(test_tcp_userdata),
        T(test_tcp_send_after_close),
    };

    size_t n = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < n; i++) {
        printf("  %s ... ", tests[i].name);
        fflush(stdout);
        tests[i].fn();
        printf("ok\n");
    }
    printf("all %zu tcp tests passed\n", n);

    platform_socket_cleanup();
    return 0;
}
