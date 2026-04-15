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

#include <stdint.h>

static void test_init_destroy(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);
    xylem_loop_destroy(loop);
}

static void test_run_exits_no_handles(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);
    ASSERT(xylem_loop_run(loop) == 0);
    xylem_loop_destroy(loop);
}

static void _on_oneshot(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                        void* ud) {
    (void)loop;
    int* count = (int*)ud;
    (*count)++;
    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);
}

static void test_timer_oneshot(void) {
    int oneshot_count = 0;

    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* timer = xylem_loop_create_timer(loop);
    ASSERT(timer != NULL);

    ASSERT(xylem_loop_start_timer(timer, _on_oneshot, &oneshot_count, 10, 0) == 0);
    ASSERT(xylem_loop_run(loop) == 0);

    ASSERT(oneshot_count == 1);

    xylem_loop_destroy(loop);
}

static void _on_repeat(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                       void* ud) {
    (void)loop;
    int* count = (int*)ud;
    (*count)++;
    if (*count >= 3) {
        xylem_loop_stop_timer(timer);
        xylem_loop_destroy_timer(timer);
    }
}

static void test_timer_repeat(void) {
    int repeat_count = 0;

    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* timer = xylem_loop_create_timer(loop);
    ASSERT(timer != NULL);

    ASSERT(xylem_loop_start_timer(timer, _on_repeat, &repeat_count, 10, 10) == 0);
    ASSERT(xylem_loop_run(loop) == 0);

    ASSERT(repeat_count == 3);

    xylem_loop_destroy(loop);
}

typedef struct {
    xylem_loop_timer_t* victim;
    int                 stopped_count;
} _stop_ctx_t;

static void _on_stopped_timer(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                              void* ud) {
    (void)loop;
    (void)timer;
    _stop_ctx_t* ctx = (_stop_ctx_t*)ud;
    ctx->stopped_count++;
}

static void _on_stopper(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                        void* ud) {
    (void)loop;
    _stop_ctx_t* ctx = (_stop_ctx_t*)ud;
    xylem_loop_stop_timer(ctx->victim);
    xylem_loop_destroy_timer(ctx->victim);
    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);
}

static void test_timer_stop(void) {
    _stop_ctx_t ctx = {0};

    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* stopper = xylem_loop_create_timer(loop);
    xylem_loop_timer_t* victim = xylem_loop_create_timer(loop);
    ASSERT(stopper != NULL);
    ASSERT(victim != NULL);

    ctx.victim = victim;

    ASSERT(xylem_loop_start_timer(stopper, _on_stopper, &ctx, 10, 0) == 0);
    ASSERT(xylem_loop_start_timer(victim, _on_stopped_timer, &ctx, 50, 0) == 0);
    ASSERT(xylem_loop_run(loop) == 0);

    ASSERT(ctx.stopped_count == 0);

    xylem_loop_destroy(loop);
}

static void _on_reset_timer(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                            void* ud) {
    (void)loop;
    uint64_t* fire_time = (uint64_t*)ud;
    *fire_time = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);
}

static void test_timer_reset(void) {
    uint64_t reset_fire_time = 0;

    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* timer = xylem_loop_create_timer(loop);
    ASSERT(timer != NULL);

    ASSERT(xylem_loop_start_timer(timer, _on_reset_timer, &reset_fire_time, 500, 0) == 0);
    ASSERT(xylem_loop_reset_timer(timer, 10) == 0);

    uint64_t before = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    ASSERT(xylem_loop_run(loop) == 0);

    ASSERT(reset_fire_time > 0);
    ASSERT(reset_fire_time - before < 200);

    xylem_loop_destroy(loop);
}

static void _on_check_now(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                          void* ud) {
    (void)loop;
    (void)ud;
    uint64_t t = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    ASSERT(t > 0);
    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);
}

static void test_loop_now(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);
    ASSERT(xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) > 0);

    xylem_loop_timer_t* timer = xylem_loop_create_timer(loop);
    ASSERT(timer != NULL);
    ASSERT(xylem_loop_start_timer(timer, _on_check_now, NULL, 1, 0) == 0);
    ASSERT(xylem_loop_run(loop) == 0);

    xylem_loop_destroy(loop);
}

static void _on_post(xylem_loop_t* loop, xylem_loop_post_t* req, void* ud) {
    (void)loop;
    (void)req;
    int* count = (int*)ud;
    (*count)++;
}

static void _on_post_trigger(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                             void* ud) {
    xylem_loop_post(loop, _on_post, ud);
    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);
}

static void test_post_same_thread(void) {
    int post_count = 0;

    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* timer = xylem_loop_create_timer(loop);
    ASSERT(timer != NULL);
    ASSERT(xylem_loop_start_timer(timer, _on_post_trigger, &post_count, 10, 0) == 0);

    ASSERT(xylem_loop_run(loop) == 0);
    ASSERT(post_count == 1);

    xylem_loop_destroy(loop);
}

typedef struct {
    xylem_loop_t*       loop;
    int                 cross_post_count;
    xylem_waitgroup_t*  ready;
} _cross_ctx_t;

static void _on_cross_post(xylem_loop_t* loop, xylem_loop_post_t* req,
                           void* ud) {
    (void)req;
    _cross_ctx_t* ctx = (_cross_ctx_t*)ud;
    ctx->cross_post_count++;
    xylem_loop_stop(loop);
}

static void _on_keepalive(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                          void* ud) {
    (void)loop;
    (void)timer;
    (void)ud;
}

static void _on_signal_ready(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                             void* ud) {
    (void)loop;
    _cross_ctx_t* ctx = (_cross_ctx_t*)ud;
    xylem_waitgroup_done(ctx->ready);
    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);
}

static int _poster_thread(void* arg) {
    _cross_ctx_t* ctx = (_cross_ctx_t*)arg;

    /* Wait until the event loop is running. */
    xylem_waitgroup_wait(ctx->ready);

    xylem_loop_post(ctx->loop, _on_cross_post, ctx);
    return 0;
}

static void test_post_cross_thread(void) {
    _cross_ctx_t ctx = {0};

    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    ctx.ready = xylem_waitgroup_create();
    ASSERT(ctx.ready != NULL);
    xylem_waitgroup_add(ctx.ready, 1);

    xylem_loop_timer_t* keepalive = xylem_loop_create_timer(ctx.loop);
    ASSERT(keepalive != NULL);
    ASSERT(xylem_loop_start_timer(keepalive, _on_keepalive, NULL, 5000, 0) == 0);

    /* Fire a 0ms timer to signal the poster thread that the loop is running. */
    xylem_loop_timer_t* ready_timer = xylem_loop_create_timer(ctx.loop);
    ASSERT(ready_timer != NULL);
    ASSERT(xylem_loop_start_timer(ready_timer, _on_signal_ready, &ctx, 0, 0) == 0);

    thrd_t thr;
    ASSERT(thrd_create(&thr, _poster_thread, &ctx) == thrd_success);

    ASSERT(xylem_loop_run(ctx.loop) == 0);
    ASSERT(ctx.cross_post_count == 1);

    thrd_join(thr, NULL);

    xylem_loop_stop_timer(keepalive);
    xylem_loop_destroy_timer(keepalive);

    xylem_waitgroup_destroy(ctx.ready);
    xylem_loop_destroy(ctx.loop);
}

static void _on_stop_loop(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                          void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

static void test_stop_from_callback(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* timer = xylem_loop_create_timer(loop);
    ASSERT(timer != NULL);
    ASSERT(xylem_loop_start_timer(timer, _on_stop_loop, NULL, 10, 0) == 0);

    ASSERT(xylem_loop_run(loop) == 0);

    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);

    xylem_loop_destroy(loop);
}

typedef struct {
    int log[3];
    int idx;
} _order_ctx_t;

static void _on_order_a(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                        void* ud) {
    (void)loop;
    _order_ctx_t* ctx = (_order_ctx_t*)ud;
    ctx->log[ctx->idx++] = 1;
    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);
}

static void _on_order_b(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                        void* ud) {
    (void)loop;
    _order_ctx_t* ctx = (_order_ctx_t*)ud;
    ctx->log[ctx->idx++] = 2;
    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);
}

static void _on_order_c(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                        void* ud) {
    (void)loop;
    _order_ctx_t* ctx = (_order_ctx_t*)ud;
    ctx->log[ctx->idx++] = 3;
    xylem_loop_stop_timer(timer);
    xylem_loop_destroy_timer(timer);
}

static void test_timer_ordering(void) {
    _order_ctx_t ctx = {0};

    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_loop_timer_t* ta = xylem_loop_create_timer(loop);
    xylem_loop_timer_t* tb = xylem_loop_create_timer(loop);
    xylem_loop_timer_t* tc = xylem_loop_create_timer(loop);
    ASSERT(ta != NULL);
    ASSERT(tb != NULL);
    ASSERT(tc != NULL);

    ASSERT(xylem_loop_start_timer(tc, _on_order_c, &ctx, 30, 0) == 0);
    ASSERT(xylem_loop_start_timer(tb, _on_order_b, &ctx, 20, 0) == 0);
    ASSERT(xylem_loop_start_timer(ta, _on_order_a, &ctx, 10, 0) == 0);

    ASSERT(xylem_loop_run(loop) == 0);

    ASSERT(ctx.idx == 3);
    ASSERT(ctx.log[0] == 1);
    ASSERT(ctx.log[1] == 2);
    ASSERT(ctx.log[2] == 3);

    xylem_loop_destroy(loop);
}

int main(void) {
    xylem_startup();

    test_init_destroy();
    test_run_exits_no_handles();
    test_timer_oneshot();
    test_timer_repeat();
    test_timer_stop();
    test_timer_reset();
    test_loop_now();
    test_post_same_thread();
    test_post_cross_thread();
    test_stop_from_callback();
    test_timer_ordering();

    xylem_cleanup();
    return 0;
}
