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
#include "utils.h"

#include <stdatomic.h>
#include <stdint.h>

#define FIRE_TARGET       5

typedef struct {
    atomic_int         fires;
    xylem_waitgroup_t* wg;
} _fire_ctx_t;

typedef struct {
    atomic_uint_least64_t fired_at_ms;
    uint64_t              armed_at_ms;
    xylem_waitgroup_t*    wg;
} _reset_ctx_t;

typedef struct {
    xylem_timer_t*     timer;
    atomic_int         fires;
    atomic_int         in_cb;
    atomic_int         reentered;
    atomic_int         done;
    xylem_waitgroup_t* wg;
} _every_overlap_ctx_t;

typedef struct {
    atomic_int            fires;
    atomic_uint_least64_t first_fire_ms;
    atomic_uint_least64_t second_fire_ms;
    xylem_waitgroup_t*    wg;
} _every_reset_ctx_t;

static xylem_timer_t* _arm_watchdog(void) {
    return xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
}

static void _after_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    _fire_ctx_t* ctx = (_fire_ctx_t*)ud;
    atomic_fetch_add(&ctx->fires, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _after_main(void* arg) {
    (void)arg;
    _fire_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();
    xylem_timer_t* t  = xylem_timer_after(30, _after_cb, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);

    ASSERT(atomic_load(&ctx.fires) == 1);

    xylem_waitgroup_destroy(ctx.wg);
}

static void test_after(void) {
    _after_main(NULL);
}

static void _cancel_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "callback fired despite cancel");
}

static void _cancel_main(void* arg) {
    (void)arg;
    xylem_timer_t* wd = _arm_watchdog();
    xylem_timer_t* t  = xylem_timer_after(1000, _cancel_cb, NULL);
    ASSERT(xylem_timer_cancel(t));
    xylem_sleep(50);

    xylem_timer_cancel(wd);
}

static void test_cancel(void) {
    _cancel_main(NULL);
}

static void _repeat_cb(xylem_timer_t* t, void* ud) {
    _fire_ctx_t* ctx = (_fire_ctx_t*)ud;
    if (atomic_fetch_add(&ctx->fires, 1) + 1 == FIRE_TARGET) {
        xylem_waitgroup_done(ctx->wg);
    } else {
        xylem_timer_reset(t, 10);
    }
}

static void _repeat_main(void* arg) {
    (void)arg;
    _fire_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();
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
}

static void test_repeat(void) {
    _repeat_main(NULL);
}

static void _every_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    _fire_ctx_t* ctx = (_fire_ctx_t*)ud;
    if (atomic_fetch_add(&ctx->fires, 1) + 1 == FIRE_TARGET) {
        xylem_waitgroup_done(ctx->wg);
    }
}

static void _every_main(void* arg) {
    (void)arg;
    _fire_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();
    xylem_timer_t* t  = xylem_timer_every(10, _every_cb, &ctx);
    ASSERT(t != NULL);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(t);

    int at_cancel = atomic_load(&ctx.fires);
    xylem_sleep(60);
    int after = atomic_load(&ctx.fires);
    ASSERT(after - at_cancel <= 1);
    ASSERT(after >= FIRE_TARGET);

    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_every(void) {
    _every_main(NULL);
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

    xylem_timer_t* wd = _arm_watchdog();

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
}

static void test_reset(void) {
    _reset_main(NULL);
}

typedef struct {
    atomic_int            fires;
    atomic_uint_least64_t first_fire_ms;
    xylem_waitgroup_t*    wg;
} _reset_repeat_ctx_t;

static void _reset_repeat_cb(xylem_timer_t* t, void* ud) {
    _reset_repeat_ctx_t* ctx = (_reset_repeat_ctx_t*)ud;
    int n = atomic_fetch_add(&ctx->fires, 1) + 1;
    if (n == 1) {
        atomic_store(&ctx->first_fire_ms,
                     xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    }
    if (n == FIRE_TARGET) {
        xylem_waitgroup_done(ctx->wg);
    } else {
        xylem_timer_reset(t, 30);
    }
}

static void _reset_repeat_main(void* arg) {
    (void)arg;
    _reset_repeat_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    atomic_init(&ctx.fires, 0);
    atomic_init(&ctx.first_fire_ms, 0);
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();

    xylem_timer_t* t = xylem_timer_after(500, _reset_repeat_cb, &ctx);

    xylem_sleep(20);
    uint64_t reset_at_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    ASSERT(xylem_timer_reset(t, 30));

    xylem_waitgroup_wait(ctx.wg);

    /**
     * What this proves: reset() shortened the original 500ms arm to 30ms.
     * Had reset not taken effect, the first fire would land near the
     * original deadline (~480ms after reset_at). Bounding only the FIRST
     * interval -- not the whole 5-fire repeat sequence -- keeps the check
     * robust on slow/loaded CI runners while still cleanly separating
     * "reset worked" (~30ms) from "reset ignored" (~480ms). The repeat
     * count is checked separately, with no timing dependency.
     */
    uint64_t first_elapsed = atomic_load(&ctx.first_fire_ms) - reset_at_ms;
    ASSERT(first_elapsed < 400);
    ASSERT(atomic_load(&ctx.fires) >= FIRE_TARGET);

    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_reset_repeat(void) {
    _reset_repeat_main(NULL);
}

static void _blocking_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    _fire_ctx_t* ctx = (_fire_ctx_t*)ud;
    xylem_sleep(20);
    atomic_fetch_add(&ctx->fires, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _blocking_main(void* arg) {
    (void)arg;
    _fire_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();
    xylem_timer_t* t  = xylem_timer_after(10, _blocking_cb, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);

    ASSERT(atomic_load(&ctx.fires) == 1);

    xylem_waitgroup_destroy(ctx.wg);
}

static void test_blocking_cb(void) {
    _blocking_main(NULL);
}

static void _every_overlap_cb(xylem_timer_t* timer, void* ud) {
    _every_overlap_ctx_t* ctx = (_every_overlap_ctx_t*)ud;
    ASSERT(timer == ctx->timer);

    if (atomic_fetch_add(&ctx->in_cb, 1) != 0) {
        atomic_store(&ctx->reentered, 1);
        if (atomic_exchange(&ctx->done, 1) == 0) {
            xylem_waitgroup_done(ctx->wg);
        }
    }

    int n = atomic_fetch_add(&ctx->fires, 1) + 1;
    xylem_sleep(20);

    if (n >= 3 && atomic_exchange(&ctx->done, 1) == 0) {
        ASSERT(xylem_timer_cancel(timer) == false);
        xylem_waitgroup_done(ctx->wg);
    }
    atomic_fetch_sub(&ctx->in_cb, 1);
}

static void _every_overlap_main(void* arg) {
    (void)arg;
    _every_overlap_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();
    ctx.timer         = xylem_timer_every(1, _every_overlap_cb, &ctx);
    ASSERT(ctx.timer != NULL);

    xylem_waitgroup_wait(ctx.wg);
    while (atomic_load(&ctx.in_cb) != 0) {
        xylem_sleep(1);
    }

    ASSERT(atomic_load(&ctx.reentered) == 0);
    ASSERT(atomic_load(&ctx.fires) >= 3);

    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_every_callbacks_do_not_overlap(void) {
    _every_overlap_main(NULL);
}

static void _every_reset_cb(xylem_timer_t* timer, void* ud) {
    _every_reset_ctx_t* ctx = (_every_reset_ctx_t*)ud;
    int n = atomic_fetch_add(&ctx->fires, 1) + 1;
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (n == 1) {
        atomic_store(&ctx->first_fire_ms, now);
        ASSERT(xylem_timer_reset(timer, 30) == false);
    } else if (n == 2) {
        atomic_store(&ctx->second_fire_ms, now);
    } else if (n == 3) {
        ASSERT(xylem_timer_cancel(timer) == false);
        xylem_waitgroup_done(ctx->wg);
    }
}

static void _every_reset_main(void* arg) {
    (void)arg;
    _every_reset_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();
    xylem_timer_t* t  = xylem_timer_every(10, _every_reset_cb, &ctx);
    ASSERT(t != NULL);

    xylem_waitgroup_wait(ctx.wg);

    uint64_t first  = atomic_load(&ctx.first_fire_ms);
    uint64_t second = atomic_load(&ctx.second_fire_ms);
    ASSERT(first > 0);
    ASSERT(second >= first);
    ASSERT(second - first >= 25);
    ASSERT(atomic_load(&ctx.fires) == 3);

    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_every_reset_and_cancel_from_callback(void) {
    _every_reset_main(NULL);
}

static void _every_reset_zero_main(void* arg) {
    (void)arg;
    _fire_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();
    xylem_timer_t* t  = xylem_timer_every(10, _every_cb, &ctx);
    ASSERT(t != NULL);
    ASSERT(xylem_timer_reset(t, 0) == false);

    xylem_waitgroup_wait(ctx.wg);
    ASSERT(atomic_load(&ctx.fires) >= FIRE_TARGET);

    xylem_timer_cancel(t);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_every_reset_rejects_zero_interval(void) {
    _every_reset_zero_main(NULL);
}

static void _null_main(void* arg) {
    (void)arg;
    xylem_timer_t* wd = _arm_watchdog();
    ASSERT(xylem_timer_every(0, _every_cb, NULL) == NULL);
    ASSERT(xylem_timer_cancel(NULL) == false);
    ASSERT(xylem_timer_reset(NULL, 10) == false);
    xylem_timer_cancel(wd);
}

static void test_null(void) {
    _null_main(NULL);
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_after();
    test_cancel();
    test_repeat();
    test_every();
    test_reset();
    test_reset_repeat();
    test_blocking_cb();
    test_every_callbacks_do_not_overlap();
    test_every_reset_and_cancel_from_callback();
    test_every_reset_rejects_zero_interval();
    test_null();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_opts_t opts = { .workers = 2 };
    xylem_run(_test_run_all, NULL, &opts);
    return 0;
}
