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

/**
 * Refcount lets a fire on one thread race safely against cancel on
 * another (and against cancel called from inside cb itself):
 *   - creator ref: dropped by cancel.
 *   - arm ref: dropped by the fire trampoline (one-shot only), or
 *     by cancel when stop() caught a pending fire or the timer is
 *     periodic.
 *   - fire ref: transient, pins t across the user cb so a racing
 *     cancel cannot free it before the post-cb field reads.
 */

#include "xylem/xylem-timer.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdlib.h>

struct xylem_timer_s {
    sched_timer_t*   st;
    xylem_timer_fn_t cb;
    void*            ud;
    bool             periodic;
    _Atomic int32_t  refcnt;
};

static void _timer_ref(xylem_timer_t* t) {
    atomic_fetch_add_explicit(&t->refcnt, 1, memory_order_relaxed);
}

static void _timer_unref(xylem_timer_t* t) {
    if (atomic_fetch_sub_explicit(
            &t->refcnt, 1, memory_order_acq_rel) == 1) {
        free(t);
    }
}

static void _timer_fire(sched_timer_t* st, void* ud) {
    (void)st;
    xylem_timer_t* t = (xylem_timer_t*)ud;
    /* Pin t so a racing cancel cannot free it before we read t->periodic. */
    _timer_ref(t);
    t->cb(t, t->ud);
    bool periodic = t->periodic;
    _timer_unref(t); /* fire ref */
    if (!periodic) {
        _timer_unref(t); /* arm ref */
    }
}

static xylem_timer_t* _timer_arm(
    uint64_t timeout_ms, uint64_t repeat_ms,
    xylem_timer_fn_t cb, void* ud) {
    scheduler_t* sched = runtime_get_scheduler();
    if (!sched || !cb) {
        return NULL;
    }

    xylem_timer_t* t = (xylem_timer_t*)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }

    t->st = sched_timer_create(sched);
    if (!t->st) {
        free(t);
        return NULL;
    }

    t->cb       = cb;
    t->ud       = ud;
    t->periodic = (repeat_ms != 0);
    atomic_store_explicit(&t->refcnt, 2, memory_order_relaxed);

    sched_timer_start(t->st, _timer_fire, t, timeout_ms, repeat_ms);
    return t;
}

xylem_timer_t* xylem_timer_after(
    uint64_t delay_ms, xylem_timer_fn_t cb, void* ud) {
    return _timer_arm(delay_ms, 0, cb, ud);
}

xylem_timer_t* xylem_timer_every(
    uint64_t period_ms, xylem_timer_fn_t cb, void* ud) {
    if (period_ms == 0) {
        return NULL;
    }
    return _timer_arm(period_ms, period_ms, cb, ud);
}

bool xylem_timer_cancel(xylem_timer_t* t) {
    if (!t) {
        return false;
    }

    bool cancelled = sched_timer_stop(t->st);
    sched_timer_destroy(t->st);
    t->st = NULL;

    /* Arm ref is ours to drop only when the fire trampoline will not
     * run (stop caught it) or will not drop it itself (periodic). */
    if (cancelled || t->periodic) {
        _timer_unref(t);
    }
    _timer_unref(t); /* creator ref */
    return cancelled;
}

bool xylem_timer_reset(xylem_timer_t* t, uint64_t delay_ms) {
    return sched_timer_reset(t->st, delay_ms);
}
