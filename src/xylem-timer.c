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
 * xylem_timer_t is a refcounted wrapper over sched_timer_t so a
 * cancel on one thread cannot free the handle while a fire is in
 * flight on another.
 *
 * Three ref roles (refcnt = 2 at creation: creator + dispatch):
 *   creator  - paired with cancel.
 *   dispatch - covers the gap between the scheduler snapshotting
 *              cb/ud under timer_lock and the trampoline taking its
 *              own fire ref; nothing in the trampoline itself can
 *              protect that window. Released by the one-shot
 *              trampoline on fire, by cancel otherwise.
 *   fire     - transient; trampoline holds it across the user cb
 *              so an in-cb self-cancel cannot free the handle
 *              between cb return and the post-cb field reads.
 *
 * xylem_timer_cancel is terminal: the caller must externally
 * serialize it against any other API on the same handle.
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
    /* Pin across cb: a racing cancel may drop creator + dispatch. */
    _timer_ref(t);
    t->cb(t, t->ud);
    _timer_unref(t); /* fire */
    if (!t->periodic) {
        _timer_unref(t); /* dispatch: one-shot trampoline owns it */
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

    /**
     * Dispatch: only ours to drop when the trampoline will not.
     *   cancelled   -> we caught the pending fire in the heap.
     *   periodic    -> trampoline never releases it.
     * For a one-shot that was already dequeued or already fired,
     * the trampoline owns the release; dropping it here would free
     * the handle before the trampoline takes its fire ref.
     */
    if (cancelled || t->periodic) {
        _timer_unref(t);
    }
    _timer_unref(t); /* creator */
    return cancelled;
}

bool xylem_timer_reset(xylem_timer_t* t, uint64_t timeout_ms) {
    if (!t) {
        return false;
    }
    /* was_active=false means a one-shot trampoline already released
     * the dispatch ref; re-take one for the fresh arm to hand off. */
    bool was_active = sched_timer_reset(t->st, timeout_ms);
    if (!was_active && !t->periodic) {
        _timer_ref(t);
    }
    return was_active;
}
