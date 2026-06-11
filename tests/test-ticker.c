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
#include "xylem/xylem-threads.h"
#include "utils.h"

#include <stdatomic.h>
#include <stdint.h>

#define SAFETY_TIMEOUT_MS 10000
#define TICK_INTERVAL_MS  20
#define TICK_TARGET       5

typedef struct {
    xylem_ticker_t*    tk;
    atomic_int         ticks;
    atomic_bool        ended;
    xylem_waitgroup_t* wg;
} _consume_ctx_t;

static void _tick_main(void* arg) {
    (void)arg;
    xylem_ticker_t* tk = xylem_ticker_create(TICK_INTERVAL_MS);
    ASSERT(tk != NULL);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);

    uint64_t prev = 0;
    for (int i = 0; i < TICK_TARGET; i++) {
        uint64_t now = xylem_ticker_recv(tk);
        ASSERT(now != 0);
        ASSERT(now >= prev);
        prev = now;
    }

    xylem_timer_cancel(wd);
    xylem_ticker_destroy(tk);
    xylem_shutdown();
}

static void test_tick(void) {
    xylem_run(_tick_main, NULL, NULL);
}

static void _invalid_main(void* arg) {
    (void)arg;
    /* Zero interval and NULL handles are rejected without aborting. */
    ASSERT(xylem_ticker_create(0) == NULL);
    ASSERT(xylem_ticker_recv(NULL) == 0);
    xylem_ticker_destroy(NULL);
    xylem_shutdown();
}

static void test_invalid(void) {
    xylem_run(_invalid_main, NULL, NULL);
}

static void _consumer(void* arg) {
    _consume_ctx_t* c = (_consume_ctx_t*)arg;
    for (;;) {
        uint64_t now = xylem_ticker_recv(c->tk);
        if (now == 0) {
            atomic_store(&c->ended, true);
            break;
        }
        atomic_fetch_add(&c->ticks, 1);
    }
    xylem_waitgroup_done(c->wg);
}

static void _destroy_main(void* arg) {
    (void)arg;
    _consume_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    ctx.tk = xylem_ticker_create(TICK_INTERVAL_MS);
    ASSERT(ctx.tk != NULL);
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);

    xylem_spawn(_consumer, &ctx);

    /**
     * Wait until the consumer has actually drained at least one tick,
     * then tear down: the parked recv must wake and return 0 so the
     * consumer loop exits. A fixed sleep here would be racy -- OS timer
     * coalescing (macOS kqueue, loaded CI runners) can stretch the first
     * tick past any fixed deadline. The safety watchdog bounds the wait
     * if ticks somehow never arrive.
     */
    while (atomic_load(&ctx.ticks) < 1) {
        xylem_sleep(TICK_INTERVAL_MS);
    }
    xylem_ticker_destroy(ctx.tk);

    xylem_waitgroup_wait(ctx.wg);
    ASSERT(atomic_load(&ctx.ended));
    ASSERT(atomic_load(&ctx.ticks) >= 1);

    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_destroy_wakes_recv(void) {
    xylem_run(_destroy_main, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* test_thread_consumer: an external OS thread drives xylem_ticker_recv */
/* directly. The recv must block the thread (not park a coroutine) and  */
/* return real tick times, proving the ticker is usable from a thread   */
/* just like the underlying timer.                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    xylem_ticker_t* tk;
    atomic_int      ticks;
    atomic_bool     ended;
    atomic_bool     ordered; /* ticks observed monotonically by the thread */
} _thr_consume_ctx_t;

static int _thread_consumer_fn(void* arg) {
    _thr_consume_ctx_t* c = (_thr_consume_ctx_t*)arg;
    uint64_t prev = 0;
    bool     ordered = true;
    for (;;) {
        /* Blocks the OS thread (context-adaptive recv) until a tick. */
        uint64_t now = xylem_ticker_recv(c->tk);
        if (now == 0) {
            atomic_store(&c->ended, true);
            break;
        }
        if (now < prev) {
            ordered = false;
        }
        prev = now;
        atomic_fetch_add(&c->ticks, 1);
    }
    atomic_store(&c->ordered, ordered);
    return 0;
}

static void _thread_consumer_main(void* arg) {
    _thr_consume_ctx_t* ctx = (_thr_consume_ctx_t*)arg;

    ctx->tk = xylem_ticker_create(TICK_INTERVAL_MS);
    ASSERT(ctx->tk != NULL);

    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);

    /* Consumer is a plain OS thread, not a coroutine. */
    thrd_t th;
    ASSERT(thrd_create(&th, _thread_consumer_fn, ctx) == thrd_success);

    /* Wait (in coroutine time) until the thread has drained enough
     * ticks, then tear down so its blocked recv wakes and returns 0. */
    while (atomic_load(&ctx->ticks) < TICK_TARGET) {
        xylem_sleep(TICK_INTERVAL_MS);
    }
    xylem_ticker_destroy(ctx->tk);

    ASSERT(thrd_join(th, NULL) == thrd_success);
    ASSERT(atomic_load(&ctx->ended));
    ASSERT(atomic_load(&ctx->ordered));
    ASSERT(atomic_load(&ctx->ticks) >= TICK_TARGET);

    xylem_timer_cancel(wd);
    xylem_shutdown();
}

static void test_thread_consumer(void) {
    _thr_consume_ctx_t ctx = {0};
    atomic_init(&ctx.ticks, 0);
    atomic_init(&ctx.ended, false);
    atomic_init(&ctx.ordered, false);
    xylem_run(_thread_consumer_main, &ctx, NULL);
}

int main(void) {
    test_tick();
    test_invalid();
    test_destroy_wakes_recv();
    test_thread_consumer();
    return 0;
}
