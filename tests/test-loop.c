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

#include "xylem/xylem-loop.h"
#include "assert.h"

static int               _oneshot_count;
static int               _repeat_count;
static int               _stopped_timer_count;
static xylem_loop_timer_t* _victim_timer;
static uint64_t          _reset_fire_time;
static int               _io_read_count;
static int               _io_write_count;
static int               _io_rearm_count;
static platform_sock_t   _io_rearm_wr;
static int               _io_stop_count;
xylem_loop_io_t*         stop_io;
static int               _post_count;
static xylem_loop_post_t _post_req;
static xylem_loop_t*     _cross_loop;
static xylem_loop_post_t _cross_post;
static int               _cross_post_count;
static int               _order_log[3];
static int               _order_idx;
static int               _combined_io_fired;
static int               _combined_timer_fired;

static void test_init_destroy(void) {
    xylem_loop_t loop;
    ASSERT(xylem_loop_init(&loop) == 0);
    xylem_loop_deinit(&loop);
}

static void test_run_exits_no_handles(void) {
    xylem_loop_t loop;
    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(loop.active_count == 0);
    ASSERT(xylem_loop_run(&loop) == 0);
    xylem_loop_deinit(&loop);
}

static void _on_oneshot(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    _oneshot_count++;
    xylem_loop_timer_close(timer);
}

static void test_timer_oneshot(void) {
    _oneshot_count = 0;

    xylem_loop_t       loop;
    xylem_loop_timer_t timer;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &timer) == 0);
    ASSERT(loop.active_count == 1);

    ASSERT(xylem_loop_timer_start(&timer, _on_oneshot, 10, 0) == 0);
    ASSERT(xylem_loop_run(&loop) == 0);

    ASSERT(_oneshot_count == 1);
    ASSERT(loop.active_count == 0);

    xylem_loop_deinit(&loop);
}

static void _on_repeat(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    _repeat_count++;
    if (_repeat_count >= 3) {
        xylem_loop_timer_close(timer);
    }
}

static void test_timer_repeat(void) {
    _repeat_count = 0;

    xylem_loop_t       loop;
    xylem_loop_timer_t timer;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &timer) == 0);

    ASSERT(xylem_loop_timer_start(&timer, _on_repeat, 10, 10) == 0);
    ASSERT(xylem_loop_run(&loop) == 0);

    ASSERT(_repeat_count == 3);

    xylem_loop_deinit(&loop);
}

static void _on_stopped_timer(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    _stopped_timer_count++;
}

static void _on_stopper(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    xylem_loop_timer_stop(_victim_timer);
    xylem_loop_timer_close(_victim_timer);
    xylem_loop_timer_close(timer);
}

static void test_timer_stop(void) {
    _stopped_timer_count = 0;

    xylem_loop_t       loop;
    xylem_loop_timer_t stopper;
    xylem_loop_timer_t victim;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &stopper) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &victim) == 0);

    _victim_timer = &victim;

    ASSERT(xylem_loop_timer_start(&stopper, _on_stopper, 10, 0) == 0);
    ASSERT(xylem_loop_timer_start(&victim, _on_stopped_timer, 50, 0) == 0);
    ASSERT(xylem_loop_run(&loop) == 0);

    ASSERT(_stopped_timer_count == 0);

    xylem_loop_deinit(&loop);
}

static void _on_reset_timer(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    _reset_fire_time = xylem_loop_now(loop);
    xylem_loop_timer_close(timer);
}

static void test_timer_reset(void) {
    _reset_fire_time = 0;

    xylem_loop_t       loop;
    xylem_loop_timer_t timer;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &timer) == 0);

    ASSERT(xylem_loop_timer_start(&timer, _on_reset_timer, 500, 0) == 0);
    ASSERT(xylem_loop_timer_reset(&timer, 10) == 0);

    uint64_t before = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    ASSERT(xylem_loop_run(&loop) == 0);

    ASSERT(_reset_fire_time > 0);
    ASSERT(_reset_fire_time - before < 200);

    xylem_loop_deinit(&loop);
}

static void _on_check_now(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    uint64_t t = xylem_loop_now(loop);
    ASSERT(t > 0);
    xylem_loop_timer_close(timer);
}

static void test_loop_now(void) {
    xylem_loop_t       loop;
    xylem_loop_timer_t timer;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_now(&loop) > 0);

    ASSERT(xylem_loop_timer_init(&loop, &timer) == 0);
    ASSERT(xylem_loop_timer_start(&timer, _on_check_now, 1, 0) == 0);
    ASSERT(xylem_loop_run(&loop) == 0);

    xylem_loop_deinit(&loop);
}

static void _on_io_readable(xylem_loop_t* loop,
                            xylem_loop_io_t* io,
                            platform_poller_op_t revents) {
    ASSERT(revents & PLATFORM_POLLER_RD_OP);
    char buf[16];
    platform_socket_recv(io->sqe.fd, buf, sizeof(buf));
    _io_read_count++;
    xylem_loop_io_close(io);
}

static void test_io_readable(void) {
    _io_read_count = 0;

    platform_sock_t pair[2];
    ASSERT(platform_socket_socketpair(0, SOCK_STREAM, 0, pair) == 0);
    platform_socket_enable_nonblocking(pair[0], true);
    platform_socket_enable_nonblocking(pair[1], true);

    xylem_loop_t    loop;
    xylem_loop_io_t io;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_io_init(&loop, &io, pair[0]) == 0);
    ASSERT(xylem_loop_io_start(&io, PLATFORM_POLLER_RD_OP, _on_io_readable) == 0);

    platform_socket_send(pair[1], "hello", 5);

    ASSERT(xylem_loop_run(&loop) == 0);
    ASSERT(_io_read_count == 1);

    xylem_loop_deinit(&loop);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void _on_io_writable(xylem_loop_t* loop,
                            xylem_loop_io_t* io,
                            platform_poller_op_t revents) {
    ASSERT(revents & PLATFORM_POLLER_WR_OP);
    _io_write_count++;
    xylem_loop_io_close(io);
}

static void test_io_writable(void) {
    _io_write_count = 0;

    platform_sock_t pair[2];
    ASSERT(platform_socket_socketpair(0, SOCK_STREAM, 0, pair) == 0);
    platform_socket_enable_nonblocking(pair[0], true);

    xylem_loop_t    loop;
    xylem_loop_io_t io;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_io_init(&loop, &io, pair[0]) == 0);
    ASSERT(xylem_loop_io_start(&io, PLATFORM_POLLER_WR_OP, _on_io_writable) == 0);

    ASSERT(xylem_loop_run(&loop) == 0);
    ASSERT(_io_write_count == 1);

    xylem_loop_deinit(&loop);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void _on_io_rearm(xylem_loop_t* loop,
                         xylem_loop_io_t* io,
                         platform_poller_op_t revents) {
    char buf[1];
    platform_socket_recv(io->sqe.fd, buf, 1);
    _io_rearm_count++;

    if (_io_rearm_count < 3) {
        platform_socket_send(_io_rearm_wr, "x", 1);
        xylem_loop_io_start(io, PLATFORM_POLLER_RD_OP, _on_io_rearm);
    } else {
        xylem_loop_io_close(io);
    }
}

static void test_io_rearm(void) {
    _io_rearm_count = 0;

    platform_sock_t pair[2];
    ASSERT(platform_socket_socketpair(0, SOCK_STREAM, 0, pair) == 0);
    platform_socket_enable_nonblocking(pair[0], true);
    platform_socket_enable_nonblocking(pair[1], true);
    _io_rearm_wr = pair[1];

    xylem_loop_t    loop;
    xylem_loop_io_t io;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_io_init(&loop, &io, pair[0]) == 0);
    ASSERT(xylem_loop_io_start(&io, PLATFORM_POLLER_RD_OP, _on_io_rearm) == 0);

    platform_socket_send(pair[1], "a", 1);

    ASSERT(xylem_loop_run(&loop) == 0);
    ASSERT(_io_rearm_count == 3);

    xylem_loop_deinit(&loop);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void _on_io_stopped(xylem_loop_t* loop,
                           xylem_loop_io_t* io,
                           platform_poller_op_t revents) {
    _io_stop_count++;
}

static void _on_stop_trigger(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    xylem_loop_io_stop(stop_io);
    xylem_loop_io_close(stop_io);
    xylem_loop_timer_close(timer);
}

static void test_io_stop(void) {
    _io_stop_count = 0;

    platform_sock_t pair[2];
    ASSERT(platform_socket_socketpair(0, SOCK_STREAM, 0, pair) == 0);
    platform_socket_enable_nonblocking(pair[0], true);

    xylem_loop_t       loop;
    xylem_loop_io_t    io;
    xylem_loop_timer_t timer;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_io_init(&loop, &io, pair[0]) == 0);
    ASSERT(xylem_loop_io_start(&io, PLATFORM_POLLER_RD_OP, _on_io_stopped) == 0);

    stop_io = &io;

    ASSERT(xylem_loop_timer_init(&loop, &timer) == 0);
    ASSERT(xylem_loop_timer_start(&timer, _on_stop_trigger, 10, 0) == 0);

    ASSERT(xylem_loop_run(&loop) == 0);
    ASSERT(_io_stop_count == 0);

    xylem_loop_deinit(&loop);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

static void _on_post(xylem_loop_t* loop, xylem_loop_post_t* req) {
    _post_count++;
}

static void _on_post_trigger(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    _post_req.cb = _on_post;
    _post_req.ud = NULL;
    xylem_loop_post(loop, &_post_req);
    xylem_loop_timer_close(timer);
}

static void test_post_same_thread(void) {
    _post_count = 0;

    xylem_loop_t       loop;
    xylem_loop_timer_t timer;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &timer) == 0);
    ASSERT(xylem_loop_timer_start(&timer, _on_post_trigger, 10, 0) == 0);

    ASSERT(xylem_loop_run(&loop) == 0);
    ASSERT(_post_count == 1);

    xylem_loop_deinit(&loop);
}

static void _on_cross_post(xylem_loop_t* loop, xylem_loop_post_t* req) {
    _cross_post_count++;
    xylem_loop_stop(loop);
}

static void _on_keepalive(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    (void)loop;
    (void)timer;
}

static int _poster_thread(void* arg) {
    xylem_loop_t* loop = arg;

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 }; /* 20ms */
    thrd_sleep(&ts, NULL);

    _cross_post.cb = _on_cross_post;
    _cross_post.ud = NULL;
    xylem_loop_post(loop, &_cross_post);
    return 0;
}

static void test_post_cross_thread(void) {
    _cross_post_count = 0;

    xylem_loop_t       loop;
    xylem_loop_timer_t keepalive;

    ASSERT(xylem_loop_init(&loop) == 0);

    ASSERT(xylem_loop_timer_init(&loop, &keepalive) == 0);
    ASSERT(xylem_loop_timer_start(&keepalive, _on_keepalive, 5000, 0) == 0);

    thrd_t thr;
    ASSERT(thrd_create(&thr, _poster_thread, &loop) == thrd_success);

    ASSERT(xylem_loop_run(&loop) == 0);
    ASSERT(_cross_post_count == 1);

    thrd_join(thr, NULL);

    xylem_loop_timer_close(&keepalive);
    loop.active_count--;

    xylem_loop_deinit(&loop);
}

static void _on_stop_loop(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    xylem_loop_stop(loop);
}

static void test_stop_from_callback(void) {
    xylem_loop_t       loop;
    xylem_loop_timer_t timer;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &timer) == 0);
    ASSERT(xylem_loop_timer_start(&timer, _on_stop_loop, 10, 0) == 0);

    ASSERT(xylem_loop_run(&loop) == 0);

    xylem_loop_timer_close(&timer);
    loop.active_count--;

    xylem_loop_deinit(&loop);
}

static void _on_order_a(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    _order_log[_order_idx++] = 1;
    xylem_loop_timer_close(timer);
}

static void _on_order_b(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    _order_log[_order_idx++] = 2;
    xylem_loop_timer_close(timer);
}

static void _on_order_c(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    _order_log[_order_idx++] = 3;
    xylem_loop_timer_close(timer);
}

static void test_timer_ordering(void) {
    _order_idx = 0;
    memset(_order_log, 0, sizeof(_order_log));

    xylem_loop_t       loop;
    xylem_loop_timer_t ta, tb, tc;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &ta) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &tb) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &tc) == 0);

    ASSERT(xylem_loop_timer_start(&tc, _on_order_c, 30, 0) == 0);
    ASSERT(xylem_loop_timer_start(&tb, _on_order_b, 20, 0) == 0);
    ASSERT(xylem_loop_timer_start(&ta, _on_order_a, 10, 0) == 0);

    ASSERT(xylem_loop_run(&loop) == 0);

    ASSERT(_order_idx == 3);
    ASSERT(_order_log[0] == 1);
    ASSERT(_order_log[1] == 2);
    ASSERT(_order_log[2] == 3);

    xylem_loop_deinit(&loop);
}

static void _on_combined_io(xylem_loop_t* loop,
                            xylem_loop_io_t* io,
                            platform_poller_op_t revents) {
    char buf[16];
    platform_socket_recv(io->sqe.fd, buf, sizeof(buf));
    _combined_io_fired = 1;
    xylem_loop_io_close(io);
}

static void _on_combined_timer(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    _combined_timer_fired = 1;
    xylem_loop_timer_close(timer);
}

static void test_io_and_timer(void) {
    _combined_io_fired    = 0;
    _combined_timer_fired = 0;

    platform_sock_t pair[2];
    ASSERT(platform_socket_socketpair(0, SOCK_STREAM, 0, pair) == 0);
    platform_socket_enable_nonblocking(pair[0], true);
    platform_socket_enable_nonblocking(pair[1], true);

    xylem_loop_t       loop;
    xylem_loop_io_t    io;
    xylem_loop_timer_t timer;

    ASSERT(xylem_loop_init(&loop) == 0);
    ASSERT(xylem_loop_io_init(&loop, &io, pair[0]) == 0);
    ASSERT(xylem_loop_io_start(&io, PLATFORM_POLLER_RD_OP, _on_combined_io) == 0);
    ASSERT(xylem_loop_timer_init(&loop, &timer) == 0);
    ASSERT(xylem_loop_timer_start(&timer, _on_combined_timer, 20, 0) == 0);

    platform_socket_send(pair[1], "x", 1);

    ASSERT(xylem_loop_run(&loop) == 0);
    ASSERT(_combined_io_fired == 1);
    ASSERT(_combined_timer_fired == 1);

    xylem_loop_deinit(&loop);
    platform_socket_close(pair[0]);
    platform_socket_close(pair[1]);
}

int main(void) {
    platform_socket_startup();

    test_init_destroy();
    test_run_exits_no_handles();
    test_timer_oneshot();
    test_timer_repeat();
    test_timer_stop();
    test_timer_reset();
    test_loop_now();
    test_io_readable();
    test_io_writable();
    test_io_rearm();
    test_io_stop();
    test_post_same_thread();
    test_post_cross_thread();
    test_stop_from_callback();
    test_timer_ordering();
    test_io_and_timer();

    platform_socket_cleanup();
    return 0;
}
