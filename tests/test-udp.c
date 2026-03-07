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

#include "xylem/xylem-udp.h"
#include "assert.h"
#include <string.h>

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
/*  Test 1: UDP echo — basic send/receive                             */
/* ------------------------------------------------------------------ */

static xylem_loop_t g_echo_loop;
static xylem_udp_t* g_echo_receiver = NULL;
static xylem_udp_t* g_echo_sender   = NULL;
static int    g_echo_read_called = 0;
static char   g_echo_data[64];
static size_t g_echo_data_len = 0;

/* Timer to trigger the send after the loop is running */
static xylem_loop_timer_t g_echo_send_timer;

static void _echo_on_read(xylem_udp_t* udp, void* data, size_t len,
                           xylem_addr_t* addr) {
    (void)udp; (void)addr;
    g_echo_read_called = 1;
    if (len < sizeof(g_echo_data)) {
        memcpy(g_echo_data, data, len);
        g_echo_data_len = len;
    }
    xylem_loop_stop(&g_echo_loop);
}

static void _echo_send_timer_cb(xylem_loop_t* loop,
                                 xylem_loop_timer_t* timer) {
    (void)loop; (void)timer;
    xylem_addr_t dest;
    xylem_addr_pton("127.0.0.1", 18081, &dest);
    xylem_udp_send(g_echo_sender, &dest, "hello", 5);
}

static void test_udp_echo(void) {
    xylem_loop_init(&g_echo_loop);
    _start_safety_timer(&g_echo_loop);

    g_echo_read_called = 0;
    g_echo_data_len    = 0;
    memset(g_echo_data, 0, sizeof(g_echo_data));

    /* Bind receiver on 127.0.0.1:18081 */
    xylem_addr_t recv_addr;
    xylem_addr_pton("127.0.0.1", 18081, &recv_addr);

    xylem_udp_handler_t recv_handler = {
        .on_read = _echo_on_read,
    };

    g_echo_receiver = xylem_udp_bind(&g_echo_loop, &recv_addr,
                                      &recv_handler);
    ASSERT(g_echo_receiver != NULL);

    /* Bind sender on 127.0.0.1:18082 */
    xylem_addr_t send_addr;
    xylem_addr_pton("127.0.0.1", 18082, &send_addr);

    xylem_udp_handler_t send_handler = {0};

    g_echo_sender = xylem_udp_bind(&g_echo_loop, &send_addr,
                                    &send_handler);
    ASSERT(g_echo_sender != NULL);

    /* Use a short timer to send after the loop starts */
    xylem_loop_timer_init(&g_echo_loop, &g_echo_send_timer);
    xylem_loop_timer_start(&g_echo_send_timer, _echo_send_timer_cb,
                           10, 0);

    xylem_loop_run(&g_echo_loop);

    ASSERT(g_echo_read_called == 1);
    ASSERT(g_echo_data_len == 5);
    ASSERT(memcmp(g_echo_data, "hello", 5) == 0);

    /* Close handles while loop can still process the closing queue.
     * xylem_udp_close frees the struct immediately, so we must stop
     * the safety timer first to avoid dangling close_nodes. */
    _stop_safety_timer();
    xylem_loop_timer_close(&g_echo_send_timer);
    xylem_loop_deinit(&g_echo_loop);
}

/* ------------------------------------------------------------------ */
/*  Test 2: UDP datagram boundary preservation                        */
/* ------------------------------------------------------------------ */

static xylem_loop_t g_dgram_loop;
static xylem_udp_t* g_dgram_receiver = NULL;
static xylem_udp_t* g_dgram_sender   = NULL;
static int    g_dgram_read_count = 0;
static size_t g_dgram_sizes[3];
static char   g_dgram_bufs[3][4];

/* Timer to send the 3 datagrams after both sockets are ready */
static xylem_loop_timer_t g_dgram_send_timer;

static void _dgram_on_read(xylem_udp_t* udp, void* data, size_t len,
                            xylem_addr_t* addr) {
    (void)udp; (void)addr;
    if (g_dgram_read_count < 3) {
        g_dgram_sizes[g_dgram_read_count] = len;
        if (len <= sizeof(g_dgram_bufs[0])) {
            memcpy(g_dgram_bufs[g_dgram_read_count], data, len);
        }
        g_dgram_read_count++;
    }
    if (g_dgram_read_count >= 3) {
        xylem_loop_stop(&g_dgram_loop);
    }
}

static void _dgram_send_timer_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer) {
    (void)loop; (void)timer;
    xylem_addr_t dest;
    xylem_addr_pton("127.0.0.1", 18083, &dest);
    xylem_udp_send(g_dgram_sender, &dest, "A", 1);
    xylem_udp_send(g_dgram_sender, &dest, "BB", 2);
    xylem_udp_send(g_dgram_sender, &dest, "CCC", 3);
}

static void test_udp_datagram_boundary(void) {
    xylem_loop_init(&g_dgram_loop);
    _start_safety_timer(&g_dgram_loop);

    g_dgram_read_count = 0;
    memset(g_dgram_sizes, 0, sizeof(g_dgram_sizes));
    memset(g_dgram_bufs, 0, sizeof(g_dgram_bufs));

    /* Bind receiver on 127.0.0.1:18083 */
    xylem_addr_t recv_addr;
    xylem_addr_pton("127.0.0.1", 18083, &recv_addr);

    xylem_udp_handler_t recv_handler = {
        .on_read = _dgram_on_read,
    };

    g_dgram_receiver = xylem_udp_bind(&g_dgram_loop, &recv_addr,
                                       &recv_handler);
    ASSERT(g_dgram_receiver != NULL);

    /* Bind sender on 127.0.0.1:18084 */
    xylem_addr_t send_addr;
    xylem_addr_pton("127.0.0.1", 18084, &send_addr);

    xylem_udp_handler_t send_handler = {0};

    g_dgram_sender = xylem_udp_bind(&g_dgram_loop, &send_addr,
                                     &send_handler);
    ASSERT(g_dgram_sender != NULL);

    /* Use a short timer to send after the loop starts */
    xylem_loop_timer_init(&g_dgram_loop, &g_dgram_send_timer);
    xylem_loop_timer_start(&g_dgram_send_timer, _dgram_send_timer_cb,
                           10, 0);

    xylem_loop_run(&g_dgram_loop);

    ASSERT(g_dgram_read_count == 3);
    ASSERT(g_dgram_sizes[0] == 1);
    ASSERT(g_dgram_sizes[1] == 2);
    ASSERT(g_dgram_sizes[2] == 3);
    ASSERT(memcmp(g_dgram_bufs[0], "A", 1) == 0);
    ASSERT(memcmp(g_dgram_bufs[1], "BB", 2) == 0);
    ASSERT(memcmp(g_dgram_bufs[2], "CCC", 3) == 0);

    _stop_safety_timer();
    xylem_loop_timer_close(&g_dgram_send_timer);
    xylem_loop_deinit(&g_dgram_loop);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                       */
/* ------------------------------------------------------------------ */

typedef void (*test_fn)(void);

typedef struct {
    const char* name;
    test_fn     fn;
} test_entry;

#define T(fn) { #fn, fn }

int main(void) {
#ifdef _WIN32
    platform_socket_startup();
#endif

    test_entry tests[] = {
        T(test_udp_echo),
        T(test_udp_datagram_boundary),
    };

    size_t n = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < n; i++) {
        printf("  %s ... ", tests[i].name);
        fflush(stdout);
        tests[i].fn();
        printf("ok\n");
    }
    printf("all %zu udp tests passed\n", n);

#ifdef _WIN32
    platform_socket_cleanup();
#endif
    return 0;
}
