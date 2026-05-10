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
 * xylem_timer_t wraps a sched_timer_t with its own refcount so a
 * cancel on one thread cannot free the handle while a fire is in
 * flight on another.
 *
 * Three reference roles (refcnt starts at 2 = creator + dispatch):
 *
 *   creator ref
 *       Paired with xylem_timer_cancel. Always dropped by cancel.
 *
 *   dispatch ref
 *       Represents "the scheduler still has a fire to hand off, or
 *       is in the middle of handing one off". Covers the hazardous
 *       window between:
 *           scheduler: lock -> dequeue -> snapshot cb/ud -> unlock
 *       and:
 *           trampoline: enter -> take fire ref
 *       because nothing in the trampoline itself can protect that
 *       gap. Ownership rules:
 *           one-shot, trampoline ran       -> trampoline releases
 *           one-shot, cancel caught in heap-> cancel releases
 *           periodic (arm stays live)      -> cancel releases
 *       The `cancelled || periodic` gate in cancel, and the matching
 *       `!periodic` drop in the trampoline, encode these rules.
 *
 *   fire ref
 *       Transient; the trampoline takes one around the user callback
 *       so a cancel (or in-cb self-cancel) that drops creator /
 *       dispatch cannot free the handle while cb is still reading
 *       its fields.
 *
 * Fire-then-reset support: a one-shot whose trampoline already ran
 * has no dispatch ref. xylem_timer_reset detects this via
 * sched_timer_reset's was_active return and re-takes one, so the
 * next fire has a dispatch ref to release.
 *
 * Concurrency contract: xylem_timer_cancel is terminal. It must be
 * externally synchronized against xylem_timer_reset and any other
 * API on the same handle. After cancel returns, the handle is
 * invalid.
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
    /* Pin t across cb; cancel may race and drop creator+dispatch. */
    _timer_ref(t);
    t->cb(t, t->ud);
    _timer_unref(t); /* fire ref */
    if (!t->periodic) {
        _timer_unref(t); /* dispatch ref: one-shot trampoline owns it */
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
     * Dispatch ref: cancel owns it when
     *   - we caught a pending fire in the heap (cancelled=true), or
     *   - the timer is periodic (trampoline never releases it).
     * Otherwise (one-shot, already dequeued or already fired), the
     * trampoline is in flight or done, and owns the release. Dropping
     * it here would free the handle out from under a trampoline that
     * has not yet reached its own fire-ref guard.
     */
    if (cancelled || t->periodic) {
        _timer_unref(t);
    }
    _timer_unref(t); /* creator ref */
    return cancelled;
}

bool xylem_timer_reset(xylem_timer_t* t, uint64_t delay_ms) {
    if (!t) {
        return false;
    }
    /**
     * sched_timer_reset reports whether the arm was still active.
     *   was_active=true   -> previous dispatch ref is still live,
     *                        reset just retargets it. No-op on refcount.
     *   was_active=false  -> must be a one-shot whose trampoline
     *                        already released the dispatch ref (or
     *                        a periodic race that is effectively UB
     *                        per the cancel-terminal contract).
     *                        Re-take a dispatch ref so the fresh arm
     *                        has one to hand off.
     */
    bool was_active = sched_timer_reset(t->st, delay_ms);
    if (!was_active && !t->periodic) {
        _timer_ref(t);
    }
    return was_active;
}
