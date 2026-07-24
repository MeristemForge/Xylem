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

#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#define TICK_INTERVAL_MS  20
#define TICK_TARGET       5
#define COALESCE_SLEEP_MS (TICK_INTERVAL_MS * 8)

typedef struct {
    xylem_ticker_t*    tk;
    atomic_int         ticks;
    atomic_bool        ended;
    xylem_waitgroup_t* wg;
} _consume_ctx_t;

static xylem_timer_t* _arm_watchdog(void) {
    return xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
}

static void _tick_main(void* arg) {
    (void)arg;
    xylem_ticker_t* tk = xylem_ticker_create(TICK_INTERVAL_MS);
    ASSERT(tk != NULL);

    xylem_timer_t* wd = _arm_watchdog();

    uint64_t prev = 0;
    for (int i = 0; i < TICK_TARGET; i++) {
        uint64_t now = xylem_ticker_recv(tk);
        ASSERT(now != 0);
        ASSERT(now >= prev);
        prev = now;
    }

    xylem_timer_destroy(wd);
    xylem_ticker_destroy(tk);
}

static void test_tick(void) {
    _tick_main(NULL);
}

static void _coalesce_main(void* arg) {
    (void)arg;
    xylem_ticker_t* tk = xylem_ticker_create(TICK_INTERVAL_MS);
    ASSERT(tk != NULL);

    xylem_timer_t* wd = _arm_watchdog();

    uint64_t first = xylem_ticker_recv(tk);
    ASSERT(first != 0);

    xylem_sleep(COALESCE_SLEEP_MS);
    uint64_t before = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    uint64_t second = xylem_ticker_recv(tk);
    ASSERT(second != 0);
    ASSERT(second >= first);
    ASSERT(before - second >= COALESCE_SLEEP_MS / 2);

    xylem_timer_destroy(wd);
    xylem_ticker_destroy(tk);
}

static void test_coalesced_tick_keeps_delivered_time(void) {
    _coalesce_main(NULL);
}

static void _invalid_main(void* arg) {
    (void)arg;
    ASSERT(xylem_ticker_create(0) == NULL);
    ASSERT(xylem_ticker_recv(NULL) == 0);
    xylem_ticker_close(NULL);
    xylem_ticker_destroy(NULL);
}

static void test_invalid(void) {
    _invalid_main(NULL);
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

static void _close_main(void* arg) {
    (void)arg;
    _consume_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    ctx.tk = xylem_ticker_create(TICK_INTERVAL_MS);
    ASSERT(ctx.tk != NULL);
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();

    xylem_spawn(_consumer, &ctx);

    while (atomic_load(&ctx.ticks) < 1) {
        xylem_sleep(TICK_INTERVAL_MS);
    }
    xylem_ticker_close(ctx.tk);

    xylem_waitgroup_wait(ctx.wg);
    ASSERT(atomic_load(&ctx.ended));
    ASSERT(atomic_load(&ctx.ticks) >= 1);

    xylem_timer_destroy(wd);
    xylem_ticker_destroy(ctx.tk);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_close_wakes_recv(void) {
    _close_main(NULL);
}

static void _close_recv_main(void* arg) {
    (void)arg;
    xylem_ticker_t* tk = xylem_ticker_create(TICK_INTERVAL_MS);
    ASSERT(tk != NULL);

    xylem_timer_t* wd = _arm_watchdog();

    xylem_ticker_close(tk);
    xylem_ticker_close(tk);
    ASSERT(xylem_ticker_recv(tk) == 0);

    xylem_timer_destroy(wd);
    xylem_ticker_destroy(tk);
}

static void test_close_is_idempotent_and_recv_returns_zero(void) {
    _close_recv_main(NULL);
}

static void test_recv_consumes_step_credit(void) {
    xylem_ticker_t* ticker = xylem_ticker_create(1);
    ASSERT(ticker != NULL);

    xylem_sleep(10);
    while (!runtime_consume_step()) {
    }
    ASSERT(xylem_ticker_recv(ticker) != 0);
    ASSERT(!runtime_consume_step());

    xylem_ticker_destroy(ticker);
}

typedef struct {
    xylem_ticker_t* tk;
    atomic_int      ticks;
    atomic_bool     ended;
    atomic_bool     ordered;
} _thr_consume_ctx_t;

static int _thread_consumer_fn(void* arg) {
    _thr_consume_ctx_t* c = (_thr_consume_ctx_t*)arg;
    uint64_t prev = 0;
    bool     ordered = true;
    for (;;) {
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

    xylem_timer_t* wd = _arm_watchdog();

    thrd_t th;
    ASSERT(thrd_create(&th, _thread_consumer_fn, ctx) == thrd_success);

    while (atomic_load(&ctx->ticks) < TICK_TARGET) {
        xylem_sleep(TICK_INTERVAL_MS);
    }
    xylem_ticker_close(ctx->tk);

    ASSERT(thrd_join(th, NULL) == thrd_success);
    ASSERT(atomic_load(&ctx->ended));
    ASSERT(atomic_load(&ctx->ordered));
    ASSERT(atomic_load(&ctx->ticks) >= TICK_TARGET);

    xylem_timer_destroy(wd);
    xylem_ticker_destroy(ctx->tk);
}

static void test_thread_consumer(void) {
    _thr_consume_ctx_t ctx = {0};
    atomic_init(&ctx.ticks, 0);
    atomic_init(&ctx.ended, false);
    atomic_init(&ctx.ordered, false);
    _thread_consumer_main(&ctx);
}

/**
 * Concurrent destroy-while-ticking soak.
 *
 * The scheduler's ud_guard must pin the ticker while an inline timer
 * callback is in flight. This probabilistic soak hammers create/destroy
 * with no consumer reference so ASan/TSan runs have a chance to catch a
 * regression in that ownership path. Set XYLEM_TICKER_SOAK_MS to run it
 * longer under sanitizers.
 */
#define RACE_DESTROYERS  8
#define RACE_INTERVAL_MS 1
#define RACE_DEFAULT_MS  1000

typedef struct {
    atomic_bool stop;
    atomic_int  destroys;
} _race_ctx_t;

static int _race_destroyer_fn(void* arg) {
    _race_ctx_t* c = (_race_ctx_t*)arg;
    int n = 0;
    while (!atomic_load(&c->stop)) {
        xylem_ticker_t* tk = xylem_ticker_create(RACE_INTERVAL_MS);
        if (!tk) {
            continue;
        }
        /**
         * Let the timer arm and start firing so a fire can be in flight
         * on a scheduler worker at the moment we tear the ticker down.
         */
        xylem_sleep(RACE_INTERVAL_MS * 2);
        xylem_ticker_destroy(tk);
        n++;
    }
    atomic_fetch_add(&c->destroys, n);
    return 0;
}

static void _race_main(void* arg) {
    (void)arg;

    uint64_t budget_ms = RACE_DEFAULT_MS;
    const char* env = getenv("XYLEM_TICKER_SOAK_MS");
    if (env) {
        budget_ms = strtoull(env, NULL, 10);
    }

    _race_ctx_t ctx;
    atomic_init(&ctx.stop, false);
    atomic_init(&ctx.destroys, 0);

    thrd_t th[RACE_DESTROYERS];
    for (int i = 0; i < RACE_DESTROYERS; i++) {
        ASSERT(thrd_create(&th[i], _race_destroyer_fn, &ctx) == thrd_success);
    }

    xylem_sleep(budget_ms);
    atomic_store(&ctx.stop, true);

    for (int i = 0; i < RACE_DESTROYERS; i++) {
        ASSERT(thrd_join(th[i], NULL) == thrd_success);
    }
    ASSERT(atomic_load(&ctx.destroys) > 0);
}

static void test_destroy_race(void) {
    /* Few workers vs many destroyer threads => oversubscription, which
     * widens the fire-dispatch window the race depends on. */
    _race_main(NULL);
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_tick();
    test_coalesced_tick_keeps_delivered_time();
    test_invalid();
    test_close_wakes_recv();
    test_close_is_idempotent_and_recv_returns_zero();
    test_recv_consumes_step_credit();
    test_thread_consumer();
    test_destroy_race();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_opts_t opts = { .workers = 2, .coro_stack_size = 0 };
    xylem_run(_test_run_all, NULL, &opts);
    return 0;
}
