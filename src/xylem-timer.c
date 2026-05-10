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
 * Public xylem_timer facade over the scheduler's sched_timer_t.
 *
 * xylem_timer_t is a wrapper struct fully owned by this module. The
 * wrapper pairs one sched_timer_t arm with user-supplied cb / ud and
 * carries its own refcount so that an in-flight fire on one thread
 * is safe against a concurrent xylem_timer_cancel() on another:
 *
 *   refcnt = 2 at creation
 *     - "creator" ref, dropped by xylem_timer_cancel()
 *     - "arm" ref, dropped by either
 *         a) the fire trampoline after the user callback returns
 *            (one-shot), or
 *         b) xylem_timer_cancel() when sched_timer_stop() caught a
 *            pending fire, or periodic is being torn down.
 *
 * Periodic timers use the scheduler's native repeat_ms, so there is
 * no self-rearm and no alive flag to synchronise between fires. The
 * arm ref is held for the life of a periodic timer and released once
 * by cancel.
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

static void _xylem_timer_unref(xylem_timer_t* t) {
    if (atomic_fetch_sub_explicit(
            &t->refcnt, 1, memory_order_acq_rel) == 1) {
        free(t);
    }
}

static void _xylem_timer_fire(sched_timer_t* st, void* ud) {
    (void)st;
    xylem_timer_t* t = (xylem_timer_t*)ud;
    t->cb(t, t->ud);
    if (!t->periodic) {
        _xylem_timer_unref(t);
    }
}

static xylem_timer_t* _xylem_timer_arm(
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

    sched_timer_start(t->st, _xylem_timer_fire, t, timeout_ms, repeat_ms);
    return t;
}

xylem_timer_t* xylem_timer_after(
    uint64_t delay_ms, xylem_timer_fn_t cb, void* ud) {
    return _xylem_timer_arm(delay_ms, 0, cb, ud);
}

xylem_timer_t* xylem_timer_every(
    uint64_t period_ms, xylem_timer_fn_t cb, void* ud) {
    if (period_ms == 0) {
        return NULL;
    }
    return _xylem_timer_arm(period_ms, period_ms, cb, ud);
}

bool xylem_timer_cancel(xylem_timer_t* t) {
    if (!t) {
        return false;
    }

    bool cancelled = sched_timer_stop(t->st);
    sched_timer_destroy(t->st);
    t->st = NULL;

    /**
     * Drop the arm ref when the caller owns it:
     *   - stop() caught a pending fire (one-shot or periodic), or
     *   - the timer is periodic (the fire trampoline never drops it).
     * For a one-shot whose cb already ran or is in flight, the
     * trampoline owns the drop. sched_timer_destroy keeps the
     * sched_timer_t alive for any cb still executing, so reading
     * t->cb / t->ud from that cb is safe.
     */
    if (cancelled || t->periodic) {
        _xylem_timer_unref(t);
    }
    _xylem_timer_unref(t); /* creator ref */
    return cancelled;
}

bool xylem_timer_reset(xylem_timer_t* t, uint64_t delay_ms) {
    /**
     * sched_timer_reset atomically removes the previous arm (if any)
     * and inserts the new one, so our single arm ref is conserved.
     */
    return sched_timer_reset(t->st, delay_ms);
}
