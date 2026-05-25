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

#include <stdatomic.h>
#include <stdint.h>

#define SAFETY_TIMEOUT_MS 10000
#define FIRE_TARGET       5

typedef struct {
    atomic_int         fires;
    xylem_waitgroup_t* wg;
} _after_ctx_t;

typedef struct {
    atomic_int         fires;
    xylem_waitgroup_t* wg;
} _repeat_ctx_t;

typedef struct {
    atomic_uint_least64_t fired_at_ms;
    uint64_t              armed_at_ms;
    xylem_waitgroup_t*    wg;
} _reset_ctx_t;

typedef struct {
    atomic_int         fires;
    xylem_waitgroup_t* wg;
} _reset_repeat_ctx_t;

static void _watchdog_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "test timed out");
}

static void _after_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    _after_ctx_t* ctx = (_after_ctx_t*)ud;
    atomic_fetch_add(&ctx->fires, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _after_main(void* arg) {
    (void)arg;
    _after_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_timer_t* t  = xylem_timer_after(30, _after_cb, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);

    ASSERT(atomic_load(&ctx.fires) == 1);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_after(void) {
    xylem_run(_after_main, NULL, NULL);
}

static void _cancel_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "callback fired despite cancel");
}

static void _cancel_main(void* arg) {
    (void)arg;
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_timer_t* t  = xylem_timer_after(1000, _cancel_cb, NULL);
    ASSERT(xylem_timer_cancel(t));
    xylem_sleep(50);

    xylem_timer_cancel(wd);
    xylem_shutdown();
}

static void test_cancel(void) {
    xylem_run(_cancel_main, NULL, NULL);
}

static void _repeat_cb(xylem_timer_t* t, void* ud) {
    _repeat_ctx_t* ctx = (_repeat_ctx_t*)ud;
    int prev = atomic_fetch_add(&ctx->fires, 1);
    if (prev + 1 == FIRE_TARGET) {
        xylem_waitgroup_done(ctx->wg);
    } else {
        xylem_timer_reset(t, 10);
    }
}

static void _repeat_main(void* arg) {
    (void)arg;
    _repeat_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_timer_t* t  = xylem_timer_after(10, _repeat_cb, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(t);

    int at_cancel = atomic_load(&ctx.fires);
    xylem_sleep(60);
    int after = atomic_load(&ctx.fires);
    ASSERT(after - at_cancel <= 2);
    ASSERT(after >= FIRE_TARGET);

    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_repeat(void) {
    xylem_run(_repeat_main, NULL, NULL);
}

static void _reset_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    _reset_ctx_t* ctx = (_reset_ctx_t*)ud;
    atomic_store(&ctx->fired_at_ms,
                 xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    xylem_waitgroup_done(ctx->wg);
}

static void _reset_main(void* arg) {
    (void)arg;
    _reset_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);

    ctx.armed_at_ms  = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    xylem_timer_t* t = xylem_timer_after(100, _reset_cb, &ctx);

    xylem_sleep(10);
    ASSERT(xylem_timer_reset(t, 200));

    xylem_waitgroup_wait(ctx.wg);
    uint64_t elapsed = atomic_load(&ctx.fired_at_ms) - ctx.armed_at_ms;
    ASSERT(elapsed >= 200);

    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_reset(void) {
    xylem_run(_reset_main, NULL, NULL);
}

static void _reset_repeat_cb(xylem_timer_t* t, void* ud) {
    _reset_repeat_ctx_t* ctx = (_reset_repeat_ctx_t*)ud;
    int prev = atomic_fetch_add(&ctx->fires, 1);
    if (prev + 1 == FIRE_TARGET) {
        xylem_waitgroup_done(ctx->wg);
    } else {
        xylem_timer_reset(t, 30);
    }
}

static void _reset_repeat_main(void* arg) {
    (void)arg;
    _reset_repeat_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);

    xylem_timer_t* t = xylem_timer_after(500, _reset_repeat_cb, &ctx);

    xylem_sleep(20);
    uint64_t reset_at_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    ASSERT(xylem_timer_reset(t, 30));

    xylem_waitgroup_wait(ctx.wg);
    uint64_t elapsed =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) - reset_at_ms;

    ASSERT(elapsed < 450);
    ASSERT(atomic_load(&ctx.fires) >= FIRE_TARGET);

    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_reset_repeat(void) {
    xylem_run(_reset_repeat_main, NULL, NULL);
}

static void _blocking_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    _after_ctx_t* ctx = (_after_ctx_t*)ud;
    xylem_sleep(20);
    atomic_fetch_add(&ctx->fires, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _blocking_main(void* arg) {
    (void)arg;
    _after_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_timer_t* t  = xylem_timer_after(10, _blocking_cb, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);

    ASSERT(atomic_load(&ctx.fires) == 1);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_blocking_cb(void) {
    xylem_run(_blocking_main, NULL, NULL);
}

int main(void) {
    test_after();
    test_cancel();
    test_repeat();
    test_reset();
    test_reset_repeat();
    test_blocking_cb();
    return 0;
}
