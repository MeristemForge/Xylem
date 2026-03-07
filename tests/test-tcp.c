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

/* ------------------------------------------------------------------ */
/*  Safety timer: stops the loop after 2 seconds to prevent hangs     */
/* ------------------------------------------------------------------ */

static void _safety_timer_cb(xylem_loop_t* loop,
                              xylem_loop_timer_t* timer) {
    (void)timer;
    fprintf(stderr, "SAFETY TIMEOUT: test hung, stopping loop\n");
    xylem_loop_stop(loop);
}

static xylem_loop_timer_t g_safety_timer;

static void _start_safety_timer(xylem_loop_t* loop) {
    xylem_loop_timer_init(loop, &g_safety_timer);
    xylem_loop_timer_start(&g_safety_timer, _safety_timer_cb, 2000, 0);
}

static void _stop_safety_timer(void) {
    xylem_loop_timer_close(&g_safety_timer);
}

/* ------------------------------------------------------------------ */
/*  Test 1: TCP echo with FRAME_DELIM                                 */
/* ------------------------------------------------------------------ */

static xylem_tcp_conn_t* g_echo_server_conn = NULL;
static xylem_tcp_conn_t* g_echo_client_conn = NULL;
static xylem_tcp_server_t* g_echo_server    = NULL;
static xylem_loop_t g_echo_loop;
static int g_echo_accept_called  = 0;
static int g_echo_connect_called = 0;
static int g_echo_read_called    = 0;
static char g_echo_received[64];
static size_t g_echo_received_len = 0;

static void _echo_srv_on_accept(xylem_tcp_conn_t* conn) {
    g_echo_server_conn = conn;
    g_echo_accept_called = 1;
}

static void _echo_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == g_echo_server_conn) g_echo_server_conn = NULL;
}

static void _echo_srv_on_read(xylem_tcp_conn_t* conn,
                               void* data, size_t len) {
    /* Echo data back with delimiter so client can parse the frame */
    char buf[128];
    if (len + 2 <= sizeof(buf)) {
        memcpy(buf, data, len);
        buf[len]     = '\r';
        buf[len + 1] = '\n';
        xylem_tcp_send(conn, buf, len + 2);
    }
}

static void _echo_cli_on_connect(xylem_tcp_conn_t* conn) {
    g_echo_connect_called = 1;
    /* Send "hello\r\n" */
    xylem_tcp_send(conn, "hello\r\n", 7);
}

static void _echo_cli_on_read(xylem_tcp_conn_t* conn,
                               void* data, size_t len) {
    (void)conn;
    g_echo_read_called = 1;
    if (len < sizeof(g_echo_received)) {
        memcpy(g_echo_received, data, len);
        g_echo_received_len = len;
    }
    /* Just stop the loop. Cleanup is handled after loop exits. */
    xylem_loop_stop(&g_echo_loop);
}

static void _echo_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == g_echo_client_conn) g_echo_client_conn = NULL;
}

static void test_tcp_echo_delim(void) {
    xylem_loop_init(&g_echo_loop);
    _start_safety_timer(&g_echo_loop);

    g_echo_accept_called  = 0;
    g_echo_connect_called = 0;
    g_echo_read_called    = 0;
    g_echo_received_len   = 0;
    g_echo_server_conn    = NULL;
    g_echo_client_conn    = NULL;
    g_echo_server         = NULL;
    memset(g_echo_received, 0, sizeof(g_echo_received));

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

    g_echo_server = xylem_tcp_listen(&g_echo_loop, &addr,
                                     &srv_handler, &opts);
    ASSERT(g_echo_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _echo_cli_on_connect,
        .on_read    = _echo_cli_on_read,
        .on_close   = _echo_cli_on_close,
    };

    g_echo_client_conn = xylem_tcp_dial(&g_echo_loop, &addr,
                                        &cli_handler, &opts);
    ASSERT(g_echo_client_conn != NULL);

    xylem_loop_run(&g_echo_loop);

    ASSERT(g_echo_accept_called == 1);
    ASSERT(g_echo_connect_called == 1);
    ASSERT(g_echo_read_called == 1);
    ASSERT(g_echo_received_len == 5);
    ASSERT(memcmp(g_echo_received, "hello", 5) == 0);

    /* Stop safety timer first, then close server.
     * xylem_tcp_server_close frees the server immediately, which
     * invalidates its IO close_node in the closing queue. */
    _stop_safety_timer();
    if (g_echo_server) { xylem_tcp_server_close(g_echo_server); g_echo_server = NULL; }

    xylem_loop_deinit(&g_echo_loop);
}

/* ------------------------------------------------------------------ */
/*  Test 2: TCP lifecycle (connect + close)                           */
/* ------------------------------------------------------------------ */

static xylem_loop_t g_life_loop;
static xylem_tcp_server_t* g_life_server = NULL;
static xylem_tcp_conn_t* g_life_srv_conn = NULL;
static int g_life_cli_connect = 0;
static int g_life_cli_close   = 0;
static int g_life_srv_accept  = 0;
static int g_life_srv_close   = 0;

static void _life_srv_on_accept(xylem_tcp_conn_t* conn) {
    g_life_srv_accept = 1;
    g_life_srv_conn = conn;
}

static void _life_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)conn; (void)err;
    g_life_srv_close = 1;
}

static void _life_cli_on_connect(xylem_tcp_conn_t* conn) {
    g_life_cli_connect = 1;
    /* Immediately close the client connection */
    xylem_tcp_close(conn);
}

static void _life_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)conn; (void)err;
    g_life_cli_close = 1;
    /* Wait a bit for server side to detect close, then stop */
}

/* Timer to check if server-side close happened, then stop loop */
static xylem_loop_timer_t g_life_check_timer;

static void _life_check_cb(xylem_loop_t* loop,
                            xylem_loop_timer_t* timer) {
    (void)timer;
    /* Close server-side conn if still open */
    if (g_life_srv_conn && !g_life_srv_close) {
        xylem_tcp_close(g_life_srv_conn);
    }
    xylem_loop_stop(loop);
}

static void test_tcp_lifecycle(void) {
    xylem_loop_init(&g_life_loop);
    _start_safety_timer(&g_life_loop);

    g_life_cli_connect = 0;
    g_life_cli_close   = 0;
    g_life_srv_accept  = 0;
    g_life_srv_close   = 0;
    g_life_srv_conn    = NULL;
    g_life_server      = NULL;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _life_srv_on_accept,
        .on_close  = _life_srv_on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type = XYLEM_TCP_FRAME_NONE;

    g_life_server = xylem_tcp_listen(&g_life_loop, &addr,
                                     &srv_handler, &opts);
    ASSERT(g_life_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _life_cli_on_connect,
        .on_close   = _life_cli_on_close,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&g_life_loop, &addr,
                                           &cli_handler, &opts);
    ASSERT(cli != NULL);

    /* Give time for server to detect the close */
    xylem_loop_timer_init(&g_life_loop, &g_life_check_timer);
    xylem_loop_timer_start(&g_life_check_timer, _life_check_cb, 200, 0);

    xylem_loop_run(&g_life_loop);

    ASSERT(g_life_cli_connect == 1);
    ASSERT(g_life_cli_close == 1);
    ASSERT(g_life_srv_accept == 1);

    _stop_safety_timer();
    xylem_loop_timer_close(&g_life_check_timer);
    if (g_life_server) { xylem_tcp_server_close(g_life_server); g_life_server = NULL; }
    xylem_loop_deinit(&g_life_loop);
}

/* ------------------------------------------------------------------ */
/*  Test 3: TCP write done callback                                   */
/* ------------------------------------------------------------------ */

static xylem_loop_t g_wd_loop;
static xylem_tcp_server_t* g_wd_server = NULL;
static xylem_tcp_conn_t* g_wd_srv_conn = NULL;
static xylem_tcp_conn_t* g_wd_cli_conn = NULL;
static int g_wd_called    = 0;
static int g_wd_status    = -1;
static size_t g_wd_len    = 0;

static void _wd_srv_on_accept(xylem_tcp_conn_t* conn) {
    g_wd_srv_conn = conn;
}

static void _wd_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == g_wd_srv_conn) g_wd_srv_conn = NULL;
}

static void _wd_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == g_wd_cli_conn) g_wd_cli_conn = NULL;
}

static void _wd_cli_on_write_done(xylem_tcp_conn_t* conn,
                                   void* data, size_t len, int status) {
    (void)data; (void)conn;
    g_wd_called = 1;
    g_wd_status = status;
    g_wd_len    = len;

    /* Stop loop — cleanup after loop exits */
    xylem_loop_stop(&g_wd_loop);
}

static void _wd_cli_on_connect(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "data", 4);
}

static void test_tcp_write_done(void) {
    xylem_loop_init(&g_wd_loop);
    _start_safety_timer(&g_wd_loop);

    g_wd_called   = 0;
    g_wd_status   = -1;
    g_wd_len      = 0;
    g_wd_srv_conn = NULL;
    g_wd_cli_conn = NULL;
    g_wd_server   = NULL;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _wd_srv_on_accept,
        .on_close  = _wd_srv_on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type = XYLEM_TCP_FRAME_NONE;

    g_wd_server = xylem_tcp_listen(&g_wd_loop, &addr,
                                   &srv_handler, &opts);
    ASSERT(g_wd_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect    = _wd_cli_on_connect,
        .on_write_done = _wd_cli_on_write_done,
        .on_close      = _wd_cli_on_close,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&g_wd_loop, &addr,
                                           &cli_handler, &opts);
    ASSERT(cli != NULL);
    g_wd_cli_conn = cli;

    xylem_loop_run(&g_wd_loop);

    ASSERT(g_wd_called == 1);
    ASSERT(g_wd_status == 0);
    ASSERT(g_wd_len == 4);

    /* Skip manual conn cleanup to avoid use-after-free in closing queue */
    _stop_safety_timer();
    if (g_wd_server) { xylem_tcp_server_close(g_wd_server); g_wd_server = NULL; }

    xylem_loop_deinit(&g_wd_loop);
}

/* ------------------------------------------------------------------ */
/*  Test 4: TCP FRAME_FIXED framing                                   */
/* ------------------------------------------------------------------ */

static xylem_loop_t g_fix_loop;
static xylem_tcp_server_t* g_fix_server = NULL;
static xylem_tcp_conn_t* g_fix_srv_conn = NULL;
static int g_fix_read_count = 0;
static char g_fix_frames[2][4];

static void _fix_srv_on_accept(xylem_tcp_conn_t* conn) {
    g_fix_srv_conn = conn;
}

static void _fix_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == g_fix_srv_conn) g_fix_srv_conn = NULL;
}

static void _fix_srv_on_read(xylem_tcp_conn_t* conn,
                              void* data, size_t len) {
    (void)conn;
    if (g_fix_read_count < 2) {
        ASSERT(len == 4);
        memcpy(g_fix_frames[g_fix_read_count], data, 4);
    }
    g_fix_read_count++;

    if (g_fix_read_count >= 2) {
        xylem_loop_stop(&g_fix_loop);
    }
}

static void _fix_cli_on_connect(xylem_tcp_conn_t* conn) {
    /* Send 8 bytes = 2 frames of 4 bytes each */
    xylem_tcp_send(conn, "ABCDEFGH", 8);
}

static xylem_tcp_conn_t* g_fix_cli_conn = NULL;

static void _fix_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == g_fix_cli_conn) g_fix_cli_conn = NULL;
}

static void test_tcp_frame_fixed(void) {
    xylem_loop_init(&g_fix_loop);
    _start_safety_timer(&g_fix_loop);

    g_fix_read_count = 0;
    g_fix_srv_conn   = NULL;
    g_fix_server     = NULL;
    memset(g_fix_frames, 0, sizeof(g_fix_frames));

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

    g_fix_server = xylem_tcp_listen(&g_fix_loop, &addr,
                                    &srv_handler, &opts);
    ASSERT(g_fix_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _fix_cli_on_connect,
        .on_close   = _fix_cli_on_close,
    };

    xylem_tcp_opts_t cli_opts = {0};
    cli_opts.framing.type = XYLEM_TCP_FRAME_NONE;

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&g_fix_loop, &addr,
                                           &cli_handler, &cli_opts);
    ASSERT(cli != NULL);
    g_fix_cli_conn = cli;

    xylem_loop_run(&g_fix_loop);

    ASSERT(g_fix_read_count == 2);
    ASSERT(memcmp(g_fix_frames[0], "ABCD", 4) == 0);
    ASSERT(memcmp(g_fix_frames[1], "EFGH", 4) == 0);

    /* Skip manual conn cleanup to avoid use-after-free in closing queue */
    _stop_safety_timer();
    if (g_fix_server) { xylem_tcp_server_close(g_fix_server); g_fix_server = NULL; }

    xylem_loop_deinit(&g_fix_loop);
}

/* ------------------------------------------------------------------ */
/*  Test 5: TCP userdata get/set                                      */
/* ------------------------------------------------------------------ */

static xylem_loop_t g_ud_loop;
static xylem_tcp_server_t* g_ud_server = NULL;
static xylem_tcp_conn_t* g_ud_srv_conn = NULL;
static xylem_tcp_conn_t* g_ud_cli_conn = NULL;
static int g_ud_value = 42;
static int g_ud_verified = 0;

static void _ud_srv_on_accept(xylem_tcp_conn_t* conn) {
    g_ud_srv_conn = conn;
    xylem_tcp_conn_set_userdata(conn, &g_ud_value);
    void* got = xylem_tcp_conn_get_userdata(conn);
    ASSERT(got == &g_ud_value);
    ASSERT(*(int*)got == 42);
    g_ud_verified = 1;

    /* Stop loop — cleanup after loop exits */
    xylem_loop_stop(&g_ud_loop);
}

static void _ud_srv_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == g_ud_srv_conn) g_ud_srv_conn = NULL;
}

static void _ud_cli_on_close(xylem_tcp_conn_t* conn, int err) {
    (void)err;
    if (conn == g_ud_cli_conn) g_ud_cli_conn = NULL;
}

static void test_tcp_userdata(void) {
    xylem_loop_init(&g_ud_loop);
    _start_safety_timer(&g_ud_loop);

    g_ud_verified  = 0;
    g_ud_server    = NULL;
    g_ud_srv_conn  = NULL;
    g_ud_cli_conn  = NULL;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _ud_srv_on_accept,
        .on_close  = _ud_srv_on_close,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type = XYLEM_TCP_FRAME_NONE;

    g_ud_server = xylem_tcp_listen(&g_ud_loop, &addr,
                                   &srv_handler, &opts);
    ASSERT(g_ud_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_close = _ud_cli_on_close,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&g_ud_loop, &addr,
                                           &cli_handler, &opts);
    ASSERT(cli != NULL);
    g_ud_cli_conn = cli;

    xylem_loop_run(&g_ud_loop);

    ASSERT(g_ud_verified == 1);

    /* Skip manual conn cleanup to avoid use-after-free in closing queue */
    _stop_safety_timer();
    if (g_ud_server) { xylem_tcp_server_close(g_ud_server); g_ud_server = NULL; }

    xylem_loop_deinit(&g_ud_loop);
}

/* ------------------------------------------------------------------ */
/*  Test 6: TCP send after close returns -1                           */
/*  Strategy: client connects, sends data to keep write queue busy,   */
/*  then calls xylem_tcp_close (CLOSING state). A subsequent send     */
/*  on the CLOSING conn should return -1.                             */
/* ------------------------------------------------------------------ */

static xylem_loop_t g_sac_loop;
static xylem_tcp_server_t* g_sac_server = NULL;
static int g_sac_send_result = 0;
static int g_sac_tested = 0;

static void _sac_srv_on_accept(xylem_tcp_conn_t* conn) {
    (void)conn;
}

static void _sac_cli_on_connect(xylem_tcp_conn_t* conn) {
    /* Enqueue a write so close doesn't immediately destroy the conn */
    xylem_tcp_send(conn, "pending", 7);
    /* Now close — conn enters CLOSING state with pending write */
    xylem_tcp_close(conn);
    /* Send on CLOSING conn should return -1 */
    g_sac_send_result = xylem_tcp_send(conn, "x", 1);
    g_sac_tested = 1;
    xylem_loop_stop(&g_sac_loop);
}

static void test_tcp_send_after_close(void) {
    xylem_loop_init(&g_sac_loop);
    _start_safety_timer(&g_sac_loop);

    g_sac_send_result = 0;
    g_sac_tested      = 0;
    g_sac_server      = NULL;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", TCP_PORT, &addr);

    xylem_tcp_handler_t srv_handler = {
        .on_accept = _sac_srv_on_accept,
    };

    xylem_tcp_opts_t opts = {0};
    opts.framing.type = XYLEM_TCP_FRAME_NONE;

    g_sac_server = xylem_tcp_listen(&g_sac_loop, &addr,
                                    &srv_handler, &opts);
    ASSERT(g_sac_server != NULL);

    xylem_tcp_handler_t cli_handler = {
        .on_connect = _sac_cli_on_connect,
    };

    xylem_tcp_conn_t* cli = xylem_tcp_dial(&g_sac_loop, &addr,
                                           &cli_handler, &opts);
    ASSERT(cli != NULL);

    xylem_loop_run(&g_sac_loop);

    ASSERT(g_sac_tested == 1);
    ASSERT(g_sac_send_result == -1);

    _stop_safety_timer();
    if (g_sac_server) { xylem_tcp_server_close(g_sac_server); g_sac_server = NULL; }
    xylem_loop_deinit(&g_sac_loop);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                       */
/* ------------------------------------------------------------------ */

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
