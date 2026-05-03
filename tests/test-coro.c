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
#include "runtime/runtime.h"
#include "assert.h"

#include <string.h>

#define SAFETY_TIMEOUT_MS 5000

static void _safety_timeout_cb(loop_t* loop,
                                loop_timer_t* timer,
                                void* ud) {
    (void)loop; (void)timer; (void)ud;
    xylem_runtime_stop();
    ASSERT(0 && "test timed out");
}

static void _start_safety_timer(void) {
    loop_timer_t* t = loop_create_timer(runtime_loop());
    loop_start_timer(t, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);
}

/* --- test: spawn runs --- */

typedef struct {
    int value;
} _coro_ctx_t;

static void _spawn_fn(void* arg) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)arg;
    ctx->value = 42;
    xylem_runtime_stop();
}

static void _test_spawn_main(void* arg) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)arg;
    _start_safety_timer();
    xylem_spawn(_spawn_fn, ctx);
}

static void test_spawn(void) {
    _coro_ctx_t ctx = { .value = 0 };
    xylem_runtime_start(_test_spawn_main, &ctx, NULL);
    ASSERT(ctx.value == 42);
}

/* --- test: sleep --- */

static void _sleep_fn(void* arg) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)arg;
    xylem_sleep(50);
    ctx->value = 99;
    xylem_runtime_stop();
}

static void _test_sleep_main(void* arg) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)arg;
    _start_safety_timer();
    xylem_spawn(_sleep_fn, ctx);
}

static void test_sleep(void) {
    _coro_ctx_t ctx = { .value = 0 };
    xylem_runtime_start(_test_sleep_main, &ctx, NULL);
    ASSERT(ctx.value == 99);
}

/* --- test: multiple coroutines ordering --- */

typedef struct {
    int order[4];
    int idx;
} _order_ctx_t;

static void _order_fn(void* arg) {
    _order_ctx_t* ctx = (_order_ctx_t*)arg;
    int id = ctx->idx++;
    xylem_sleep((uint64_t)(id * 20 + 10));
    ctx->order[id] = id + 1;
    if (id == 2) xylem_runtime_stop();
}

static void _test_multiple_coros_main(void* arg) {
    _order_ctx_t* ctx = (_order_ctx_t*)arg;
    _start_safety_timer();
    xylem_spawn(_order_fn, ctx);
    xylem_spawn(_order_fn, ctx);
    xylem_spawn(_order_fn, ctx);
}

static void test_multiple_coros(void) {
    _order_ctx_t ctx = { .idx = 0 };
    memset(ctx.order, 0, sizeof(ctx.order));
    xylem_runtime_start(_test_multiple_coros_main, &ctx, NULL);
    ASSERT(ctx.order[0] == 1);
    ASSERT(ctx.order[1] == 2);
    ASSERT(ctx.order[2] == 3);
}

/* --- test: channel send/recv on same loop --- */

typedef struct {
    xylem_channel_t* ch;
    int              received;
} _chan_ctx_t;

static void _chan_sender(void* arg) {
    _chan_ctx_t* ctx = (_chan_ctx_t*)arg;
    static int msg_val = 123;
    xylem_sleep(30);
    xylem_channel_send(ctx->ch, &msg_val);
}

static void _chan_receiver(void* arg) {
    _chan_ctx_t* ctx = (_chan_ctx_t*)arg;
    int* val = (int*)xylem_channel_recv(ctx->ch);
    ASSERT(val != NULL);
    ASSERT(*val == 123);
    ctx->received = 1;
    xylem_runtime_stop();
}

static void _test_channel_main(void* arg) {
    _chan_ctx_t* ctx = (_chan_ctx_t*)arg;
    _start_safety_timer();

    ctx->ch = xylem_channel_create();
    ASSERT(ctx->ch != NULL);

    xylem_spawn(_chan_receiver, ctx);
    xylem_spawn(_chan_sender, ctx);
}

static void test_channel(void) {
    _chan_ctx_t ctx = { .received = 0 };
    xylem_runtime_start(_test_channel_main, &ctx, NULL);
    ASSERT(ctx.received == 1);
    xylem_channel_destroy(ctx.ch);
}

/* --- test: channel cross-thread --- */

#include "runtime/c11-threads.h"

typedef struct {
    xylem_channel_t* ch;
    int              msg_count;
} _thread_ctx_t;

static int _sender_thread(void* arg) {
    _thread_ctx_t* tctx = (_thread_ctx_t*)arg;
    static int values[] = {10, 20, 30};

    for (int i = 0; i < tctx->msg_count; i++) {
        xylem_channel_send(tctx->ch, &values[i]);
    }
    return 0;
}

typedef struct {
    xylem_channel_t* ch;
    int              sum;
} _cross_ctx_t;

static void _cross_receiver(void* arg) {
    _cross_ctx_t* ctx = (_cross_ctx_t*)arg;
    int count = 0;
    while (count < 3) {
        int* val = (int*)xylem_channel_recv(ctx->ch);
        if (!val) break;
        ctx->sum += *val;
        count++;
    }
    xylem_runtime_stop();
}

typedef struct {
    _cross_ctx_t*  cross;
    _thread_ctx_t* thread;
} _cross_test_args_t;

static void _test_channel_cross_main(void* arg) {
    _cross_test_args_t* a = (_cross_test_args_t*)arg;
    _start_safety_timer();

    a->cross->ch = xylem_channel_create();
    ASSERT(a->cross->ch != NULL);
    a->thread->ch = a->cross->ch;

    xylem_spawn(_cross_receiver, a->cross);

    thrd_t th;
    thrd_create(&th, _sender_thread, a->thread);
    thrd_join(th, NULL);
}

static void test_channel_cross_thread(void) {
    _cross_ctx_t cross = { .sum = 0 };
    _thread_ctx_t tctx = { .msg_count = 3 };
    _cross_test_args_t args = { .cross = &cross, .thread = &tctx };

    xylem_runtime_start(_test_channel_cross_main, &args, NULL);
    ASSERT(cross.sum == 60);
    xylem_channel_destroy(cross.ch);
}

int main(void) {

    test_spawn();
    test_sleep();
    test_multiple_coros();
    test_channel();
    test_channel_cross_thread();

    return 0;
}
