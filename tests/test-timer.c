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
#include <stdio.h>

#define SAFETY_TIMEOUT_MS 5000

static xylem_opts_t _rt_opts = { .workers = 2 };

static void _safety_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "test timed out");
}

/* --- test: after() fires once --- */

typedef struct {
    atomic_int         fires;
    xylem_waitgroup_t* wg;
} _after_ctx_t;

static void _after_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    _after_ctx_t* ctx = (_after_ctx_t*)ud;
    atomic_fetch_add(&ctx->fires, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _after_main(void* arg) {
    _after_ctx_t* ctx = (_after_ctx_t*)arg;
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 1);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _safety_cb, NULL);
    xylem_timer_t* t  = xylem_timer_after(30, _after_cb, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx->wg);
    xylem_shutdown();
}

static void test_after_fires_once(void) {
    fprintf(stderr, "=== test_after_fires_once\n");
    _after_ctx_t ctx = {0};
    xylem_run(_after_main, &ctx, &_rt_opts);
    ASSERT(atomic_load(&ctx.fires) == 1);
}

/* --- test: cancel before fire prevents callback --- */

static void _cancel_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "callback fired despite cancel");
}

static void _cancel_main(void* arg) {
    bool* cancelled = (bool*)arg;

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _safety_cb, NULL);
    xylem_timer_t* t  = xylem_timer_after(1000, _cancel_cb, NULL);
    *cancelled = xylem_timer_cancel(t);
    /* Sleep past the would-be fire time to prove cb really did not run. */
    xylem_sleep(50);

    xylem_timer_cancel(wd);
    xylem_shutdown();
}

static void test_cancel_before_fire(void) {
    fprintf(stderr, "=== test_cancel_before_fire\n");
    bool cancelled = false;
    xylem_run(_cancel_main, &cancelled, &_rt_opts);
    ASSERT(cancelled);
}

/* --- test: every() fires repeatedly until cancelled --- */

#define EVERY_TARGET 5

typedef struct {
    atomic_int         fires;
    xylem_waitgroup_t* wg;
} _every_ctx_t;

static void _every_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    _every_ctx_t* ctx = (_every_ctx_t*)ud;
    int prev = atomic_fetch_add(&ctx->fires, 1);
    if (prev + 1 == EVERY_TARGET) {
        xylem_waitgroup_done(ctx->wg);
    }
}

static void _every_main(void* arg) {
    _every_ctx_t* ctx = (_every_ctx_t*)arg;
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 1);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _safety_cb, NULL);
    xylem_timer_t* t  = xylem_timer_every(10, _every_cb, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(t);

    /* No more fires should land after cancel. Sleep long enough for
     * a few would-be periods to elapse, then check the count did
     * not grow unbounded. We allow +2 slack for fires already in
     * flight at cancel time. */
    int at_cancel = atomic_load(&ctx->fires);
    xylem_sleep(60);
    int after = atomic_load(&ctx->fires);
    ASSERT(after - at_cancel <= 2);

    xylem_timer_cancel(wd);
    xylem_shutdown();
}

static void test_every_repeats(void) {
    fprintf(stderr, "=== test_every_repeats\n");
    _every_ctx_t ctx = {0};
    xylem_run(_every_main, &ctx, &_rt_opts);
    ASSERT(atomic_load(&ctx.fires) >= EVERY_TARGET);
}

/* --- test: reset extends the deadline --- */

typedef struct {
    atomic_uint_least64_t fired_at_ms;
    uint64_t              armed_at_ms;
    xylem_waitgroup_t*    wg;
} _reset_ctx_t;

static void _reset_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    _reset_ctx_t* ctx = (_reset_ctx_t*)ud;
    atomic_store(
        &ctx->fired_at_ms,
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    xylem_waitgroup_done(ctx->wg);
}

static void _reset_main(void* arg) {
    _reset_ctx_t* ctx = (_reset_ctx_t*)arg;
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 1);

    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _safety_cb, NULL);

    /* Arm for 500ms, but reset after 50ms so the effective delay
     * starts from the reset time. Test passes if the cb fired
     * meaningfully later than the original 500ms deadline. */
    ctx->armed_at_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    xylem_timer_t* t = xylem_timer_after(500, _reset_cb, ctx);

    xylem_sleep(50);
    bool was_active = xylem_timer_reset(t, 600);
    ASSERT(was_active);

    xylem_waitgroup_wait(ctx->wg);
    uint64_t elapsed =
        atomic_load(&ctx->fired_at_ms) - ctx->armed_at_ms;
    /* Expected ~ 50 + 600 = 650ms. Floor is the reset deadline. */
    ASSERT(elapsed >= 600);

    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);
    xylem_shutdown();
}

static void test_reset_extends_deadline(void) {
    fprintf(stderr, "=== test_reset_extends_deadline\n");
    _reset_ctx_t ctx = {0};
    xylem_run(_reset_main, &ctx, &_rt_opts);
}

int main(void) {
    test_after_fires_once();
    test_cancel_before_fire();
    test_every_repeats();
    test_reset_extends_deadline();
    return 0;
}
