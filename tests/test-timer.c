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
#include "xylem/xylem-threads.h"

#include "assert.h"
#include "utils.h"

#include <stdatomic.h>
#include <stdint.h>

#define SAFETY_TIMEOUT_MS 10000
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
    scheduler_timer_t* timer;
    atomic_int         dispatched;
    atomic_int         old_fires;
    atomic_int         new_fires;
    xylem_waitgroup_t* wg;
} _snapshot_ctx_t;

typedef struct {
    scheduler_timer_t* timer;
    atomic_int         fires;
    atomic_int         in_cb;
    atomic_int         reentered;
    atomic_int         done;
    xylem_waitgroup_t* wg;
} _sched_repeat_ctx_t;

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
    xylem_timer_t* wd = _arm_watchdog();
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
    xylem_shutdown();
}

static void test_repeat(void) {
    xylem_run(_repeat_main, NULL, NULL);
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
    xylem_shutdown();
}

static void test_every(void) {
    xylem_run(_every_main, NULL, NULL);
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
    xylem_shutdown();
}

static void test_reset(void) {
    xylem_run(_reset_main, NULL, NULL);
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
    xylem_shutdown();
}

static void test_reset_repeat(void) {
    xylem_run(_reset_repeat_main, NULL, NULL);
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
    xylem_shutdown();
}

static void test_blocking_cb(void) {
    xylem_run(_blocking_main, NULL, NULL);
}

static void _snapshot_ud_ref(void* ud) {
    _snapshot_ctx_t* ctx = (_snapshot_ctx_t*)ud;
    atomic_store_explicit(&ctx->dispatched, 1, memory_order_release);
}

static void _snapshot_ud_unref(void* ud) {
    (void)ud;
}

static void _snapshot_old_cb(scheduler_timer_t* t, void* ud) {
    (void)t;
    _snapshot_ctx_t* ctx = (_snapshot_ctx_t*)ud;
    atomic_fetch_add(&ctx->old_fires, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _snapshot_new_cb(scheduler_timer_t* t, void* ud) {
    (void)t;
    _snapshot_ctx_t* ctx = (_snapshot_ctx_t*)ud;
    atomic_fetch_add(&ctx->new_fires, 1);
}

static int _snapshot_rearm_thread(void* arg) {
    _snapshot_ctx_t* ctx = (_snapshot_ctx_t*)arg;
    while (!atomic_load_explicit(&ctx->dispatched, memory_order_acquire)) {
        thrd_yield();
    }
    ASSERT(scheduler_timer_start(
               ctx->timer, _snapshot_new_cb, ctx, SAFETY_TIMEOUT_MS, 0)
           == 0);
    return 0;
}

static void _snapshot_main(void* arg) {
    (void)arg;
    _snapshot_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();
    ctx.timer         = scheduler_timer_create(runtime_get_scheduler());
    ASSERT(ctx.timer != NULL);
    scheduler_timer_set_spawn(ctx.timer, true);
    scheduler_timer_set_ud_guard(
        ctx.timer, _snapshot_ud_ref, _snapshot_ud_unref);

    thrd_t thr;
    ASSERT(thrd_create(&thr, _snapshot_rearm_thread, &ctx) == thrd_success);
    ASSERT(scheduler_timer_start(
               ctx.timer, _snapshot_old_cb, &ctx, 1, 0)
           == 0);

    xylem_waitgroup_wait(ctx.wg);
    thrd_join(thr, NULL);

    ASSERT(atomic_load(&ctx.old_fires) == 1);
    ASSERT(atomic_load(&ctx.new_fires) == 0);

    scheduler_timer_destroy(ctx.timer);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_fire_snapshots_callback(void) {
    for (int round = 0; round < 100; round++) {
        xylem_opts_t opts = { .workers = 1 };
        xylem_run(_snapshot_main, NULL, &opts);
    }
}

static void _repeat_spawn_cb(scheduler_timer_t* timer, void* ud) {
    _sched_repeat_ctx_t* ctx = (_sched_repeat_ctx_t*)ud;
    ASSERT(timer == ctx->timer);

    if (atomic_fetch_add(&ctx->in_cb, 1) != 0) {
        atomic_store(&ctx->reentered, 1);
        if (atomic_exchange(&ctx->done, 1) == 0) {
            scheduler_timer_stop(ctx->timer);
            xylem_waitgroup_done(ctx->wg);
        }
    }

    int n = atomic_fetch_add(&ctx->fires, 1) + 1;
    xylem_sleep(20);

    if (n >= 3 && atomic_exchange(&ctx->done, 1) == 0) {
        scheduler_timer_stop(ctx->timer);
        xylem_waitgroup_done(ctx->wg);
    }
    atomic_fetch_sub(&ctx->in_cb, 1);
}

static void _repeat_spawn_main(void* arg) {
    (void)arg;
    _sched_repeat_ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);

    xylem_timer_t* wd = _arm_watchdog();
    ctx.timer         = scheduler_timer_create(runtime_get_scheduler());
    ASSERT(ctx.timer != NULL);
    scheduler_timer_set_spawn(ctx.timer, true);
    ASSERT(scheduler_timer_start(
               ctx.timer, _repeat_spawn_cb, &ctx, 1, 1)
           == 0);

    xylem_waitgroup_wait(ctx.wg);
    scheduler_timer_stop(ctx.timer);
    while (atomic_load(&ctx.in_cb) != 0) {
        xylem_sleep(1);
    }

    ASSERT(atomic_load(&ctx.reentered) == 0);
    ASSERT(atomic_load(&ctx.fires) >= 3);

    scheduler_timer_destroy(ctx.timer);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_spawn_repeat_callbacks_do_not_overlap(void) {
    xylem_opts_t opts = { .workers = 2 };
    xylem_run(_repeat_spawn_main, NULL, &opts);
}

static void _null_main(void* arg) {
    (void)arg;
    xylem_timer_t* wd = _arm_watchdog();
    ASSERT(xylem_timer_every(0, _every_cb, NULL) == NULL);
    ASSERT(xylem_timer_cancel(NULL) == false);
    ASSERT(xylem_timer_reset(NULL, 10) == false);
    xylem_timer_cancel(wd);
    xylem_shutdown();
}

static void test_null(void) {
    xylem_run(_null_main, NULL, NULL);
}

int main(void) {
    test_after();
    test_cancel();
    test_repeat();
    test_every();
    test_reset();
    test_reset_repeat();
    test_blocking_cb();
    test_fire_snapshots_callback();
    test_spawn_repeat_callbacks_do_not_overlap();
    test_null();
    return 0;
}
