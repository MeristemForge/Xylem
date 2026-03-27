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

static xylem_loop_timer_t* _safety_timer;

/* Test 1: UDP echo */
static xylem_loop_t* _echo_loop;
static xylem_udp_t*  _echo_receiver = NULL;
static xylem_udp_t*  _echo_sender   = NULL;
static int    _echo_read_called = 0;
static char   _echo_data[64];
static size_t _echo_data_len = 0;
static xylem_loop_timer_t* _echo_send_timer;

/* Test 2: UDP datagram boundary */
static xylem_loop_t* _dgram_loop;
static xylem_udp_t*  _dgram_receiver = NULL;
static xylem_udp_t*  _dgram_sender   = NULL;
static int    _dgram_read_count = 0;
static size_t _dgram_sizes[3];
static char   _dgram_bufs[3][4];
static xylem_loop_timer_t* _dgram_send_timer;

static void _safety_timer_cb(xylem_loop_t* loop,
                              xylem_loop_timer_t* timer,
                              void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

static void _start_safety_timer(xylem_loop_t* loop) {
    _safety_timer = xylem_loop_create_timer(loop, NULL);
    xylem_loop_start_timer(_safety_timer, _safety_timer_cb, 2000, 0);
}

static void _stop_safety_timer(void) {
    xylem_loop_destroy_timer(_safety_timer);
}

static void _echo_on_read(xylem_udp_t* udp, void* data, size_t len,
                           xylem_addr_t* addr) {
    (void)udp; (void)addr;
    _echo_read_called = 1;
    if (len < sizeof(_echo_data)) {
        memcpy(_echo_data, data, len);
        _echo_data_len = len;
    }
    xylem_loop_stop(_echo_loop);
}

static void _echo_send_timer_cb(xylem_loop_t* loop,
                                 xylem_loop_timer_t* timer,
                                 void* ud) {
    (void)loop; (void)timer; (void)ud;
    xylem_addr_t dest;
    xylem_addr_pton("127.0.0.1", 18081, &dest);
    xylem_udp_send(_echo_sender, &dest, "hello", 5);
}

static void test_udp_echo(void) {
    _echo_loop = xylem_loop_create();
    ASSERT(_echo_loop != NULL);
    _start_safety_timer(_echo_loop);

    _echo_read_called = 0;
    _echo_data_len    = 0;
    memset(_echo_data, 0, sizeof(_echo_data));

    xylem_addr_t recv_addr;
    xylem_addr_pton("127.0.0.1", 18081, &recv_addr);

    xylem_udp_handler_t recv_handler = {
        .on_read = _echo_on_read,
    };

    _echo_receiver = xylem_udp_listen(_echo_loop, &recv_addr,
                                      &recv_handler);
    ASSERT(_echo_receiver != NULL);

    xylem_addr_t send_addr;
    xylem_addr_pton("127.0.0.1", 18082, &send_addr);

    xylem_udp_handler_t send_handler = {0};

    _echo_sender = xylem_udp_listen(_echo_loop, &send_addr,
                                    &send_handler);
    ASSERT(_echo_sender != NULL);

    _echo_send_timer = xylem_loop_create_timer(_echo_loop, NULL);
    ASSERT(_echo_send_timer != NULL);
    xylem_loop_start_timer(_echo_send_timer, _echo_send_timer_cb,
                           10, 0);

    xylem_loop_run(_echo_loop);

    ASSERT(_echo_read_called == 1);
    ASSERT(_echo_data_len == 5);
    ASSERT(memcmp(_echo_data, "hello", 5) == 0);

    _stop_safety_timer();
    xylem_loop_destroy_timer(_echo_send_timer);
    xylem_loop_destroy(_echo_loop);
}

static void _dgram_on_read(xylem_udp_t* udp, void* data, size_t len,
                            xylem_addr_t* addr) {
    (void)udp; (void)addr;
    if (_dgram_read_count < 3) {
        _dgram_sizes[_dgram_read_count] = len;
        if (len <= sizeof(_dgram_bufs[0])) {
            memcpy(_dgram_bufs[_dgram_read_count], data, len);
        }
        _dgram_read_count++;
    }
    if (_dgram_read_count >= 3) {
        xylem_loop_stop(_dgram_loop);
    }
}

static void _dgram_send_timer_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer,
                                  void* ud) {
    (void)loop; (void)timer; (void)ud;
    xylem_addr_t dest;
    xylem_addr_pton("127.0.0.1", 18083, &dest);
    xylem_udp_send(_dgram_sender, &dest, "A", 1);
    xylem_udp_send(_dgram_sender, &dest, "BB", 2);
    xylem_udp_send(_dgram_sender, &dest, "CCC", 3);
}

static void test_udp_datagram_boundary(void) {
    _dgram_loop = xylem_loop_create();
    ASSERT(_dgram_loop != NULL);
    _start_safety_timer(_dgram_loop);

    _dgram_read_count = 0;
    memset(_dgram_sizes, 0, sizeof(_dgram_sizes));
    memset(_dgram_bufs, 0, sizeof(_dgram_bufs));

    xylem_addr_t recv_addr;
    xylem_addr_pton("127.0.0.1", 18083, &recv_addr);

    xylem_udp_handler_t recv_handler = {
        .on_read = _dgram_on_read,
    };

    _dgram_receiver = xylem_udp_listen(_dgram_loop, &recv_addr,
                                       &recv_handler);
    ASSERT(_dgram_receiver != NULL);

    xylem_addr_t send_addr;
    xylem_addr_pton("127.0.0.1", 18084, &send_addr);

    xylem_udp_handler_t send_handler = {0};

    _dgram_sender = xylem_udp_listen(_dgram_loop, &send_addr,
                                     &send_handler);
    ASSERT(_dgram_sender != NULL);

    _dgram_send_timer = xylem_loop_create_timer(_dgram_loop, NULL);
    ASSERT(_dgram_send_timer != NULL);
    xylem_loop_start_timer(_dgram_send_timer, _dgram_send_timer_cb,
                           10, 0);

    xylem_loop_run(_dgram_loop);

    ASSERT(_dgram_read_count == 3);
    ASSERT(_dgram_sizes[0] == 1);
    ASSERT(_dgram_sizes[1] == 2);
    ASSERT(_dgram_sizes[2] == 3);
    ASSERT(memcmp(_dgram_bufs[0], "A", 1) == 0);
    ASSERT(memcmp(_dgram_bufs[1], "BB", 2) == 0);
    ASSERT(memcmp(_dgram_bufs[2], "CCC", 3) == 0);

    _stop_safety_timer();
    xylem_loop_destroy_timer(_dgram_send_timer);
    xylem_loop_destroy(_dgram_loop);
}

int main(void) {
    xylem_startup();

    test_udp_echo();
    test_udp_datagram_boundary();

    xylem_cleanup();
    return 0;
}
