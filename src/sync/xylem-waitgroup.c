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

#include "xylem/sync/xylem-waitgroup.h"

#include "xylem/xylem-logger.h"

#include "container/list.h"
#include "platform/platform-futex.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Cross-context countdown latch (Go's sync.WaitGroup).
 *
 * `cnt` is the latch counter; the predicate every waiter blocks on is
 * "cnt == 0". The two waiter kinds block differently, mirroring
 * xylem_sem / xylem_mutex:
 *
 *   - Coroutines queue FIFO on `co_waiters` and park on the scheduler.
 *     done()-at-zero drains the list and reschedules each, with no kernel
 *     round-trip.
 *
 *   - OS threads do not queue: they sleep on `gate`, a generation word
 *     that done()-at-zero bumps and broadcasts. The futex value-compare
 *     against the generation snapshotted under the guard closes the
 *     lost-wakeup race (a done() that opens the latch after the snapshot
 *     but before the sleep changes `gate`, so the wait returns at once).
 *     `thr_waiters` tells done() whether a broadcast is needed, so a
 *     coroutine-only workload pays no syscall.
 *
 * Lifetime is the delicate part. wait() returning lets its caller
 * destroy() -> free(wg), and a waiter may learn the latch is open either
 * by being woken by done() or by independently observing cnt == 0 (e.g. a
 * wait() that starts after the final done()). So the invariant is: a
 * waiter only ever inspects cnt -- the read that may lead it to return and
 * free wg -- while holding `guard`, and done() performs *every* access to
 * wg (the decrement to zero, the drain, the gate bump, the broadcast)
 * under that same guard. A waiter therefore cannot observe the open latch
 * and free wg until done() has released the guard, by which point done()'s
 * only remaining act is rescheduling the drained coroutines -- which
 * touches the private drained list and the still-parked frames, never wg.
 *
 * This is why threads, unlike in xylem_sem, also take the guard here: the
 * counter being driven to exactly zero is a one-shot edge that frees the
 * object, not a token a late barger can simply re-read. done() keeps that
 * edge inside the guard -- it CAS-decrements any value > 1 lock-free, and
 * routes only the final 1 -> 0 transition through the guard -- so the open
 * latch becomes observable to a waiter only while done() holds the guard.
 *
 * `cnt` is size_t (the public counter width); a futex word must be 32-bit,
 * so threads block on the separate `gate` generation rather than on `cnt`.
 *
 * Reuse (add() after the latch reached zero) follows Go's contract: such
 * add()s must happen-after all prior wait()s returned.
 */
struct xylem_waitgroup_s {
    atomic_size_t    cnt;
    _Atomic uint32_t gate;        /* generation; bumped when latch opens  */
    _Atomic int32_t  thr_waiters; /* OS threads blocked on the futex       */
    spin_t           guard;       /* serialises the decrement + wake path  */
    list_t           co_waiters;  /* coroutine waiters, FIFO               */
};

/**
 * Coroutine waiter record, embedded in the waiting coroutine's wait()
 * frame (which stays alive while parked, so done() may read it).
 */
typedef struct _wg_co_waiter_s {
    list_node_t  node;
    mco_coro*    co;
    scheduler_t* sched;
} _wg_co_waiter_t;

typedef struct _wg_park_ctx_s {
    xylem_waitgroup_t* wg;
    _wg_co_waiter_t*   w;
} _wg_park_ctx_t;

/**
 * Park callback, run once the coroutine has actually suspended. Under the
 * guard -- the same one done() holds across its whole critical section --
 * enqueue if the latch is still closed, or decline the park (false) if
 * done() already opened it. Declining makes wait() return inline; reading
 * cnt here under the guard is what keeps that return ordered after done()
 * has finished touching wg.
 */
static bool _wg_park_cb(mco_coro* co, void* arg) {
    _wg_park_ctx_t*    ctx = (_wg_park_ctx_t*)arg;
    xylem_waitgroup_t* wg  = ctx->wg;

    /* co is known only now that the coroutine has actually suspended. */
    ctx->w->co = co;

    spin_lock(&wg->guard);
    if (atomic_load_explicit(&wg->cnt, memory_order_acquire) == 0) {
        spin_unlock(&wg->guard);
        return false; /* latch already open: decline the park, run inline */
    }
    list_insert_tail(&wg->co_waiters, &ctx->w->node);
    spin_unlock(&wg->guard);
    return true;
}

/**
 * Reschedule every coroutine in a drained list, in FIFO order. Reads each
 * node's successor before waking it, since the woken coroutine's frame may
 * vanish on resume.
 */
static void _wg_wake_all(list_t* drained) {
    list_node_t* sentinel = list_sentinel(drained);
    list_node_t* n        = list_head(drained);
    while (n) {
        list_node_t* next = list_next(n);
        if (next == sentinel) {
            next = NULL;
        }
        _wg_co_waiter_t* w = list_entry(n, _wg_co_waiter_t, node);
        scheduler_schedule(w->sched, w->co);
        n = next;
    }
}

xylem_waitgroup_t* xylem_waitgroup_create(void) {
    xylem_waitgroup_t* wg =
        (xylem_waitgroup_t*)calloc(1, sizeof(xylem_waitgroup_t));
    if (!wg) {
        return NULL;
    }
    atomic_init(&wg->cnt, 0);
    atomic_init(&wg->gate, 0);
    atomic_init(&wg->thr_waiters, 0);
    spin_init(&wg->guard);
    list_init(&wg->co_waiters);
    return wg;
}

void xylem_waitgroup_destroy(xylem_waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    free(wg);
}

void xylem_waitgroup_add(xylem_waitgroup_t* wg, size_t delta) {
    /* Lock-free: neither parks nor wakes, so safe from any context. */
    atomic_fetch_add(&wg->cnt, delta);
}

void xylem_waitgroup_done(xylem_waitgroup_t* wg) {
    list_t drained;
    list_init(&drained);

    /**
     * Fast path: a done() that does not drive the counter to zero touches
     * nothing but the counter. Its decrement is ordered (in cnt's
     * modification order) before the final 1 -> 0 decrement that any
     * waiter's "latch open" observation reads, so it happens-before any
     * free of wg and needs no guard. Only the last decrement opens the
     * latch and must run under the guard, so a waiter cannot observe the
     * open latch and free wg while we still touch it.
     *
     * The invariant that keeps this safe is "never let the counter reach
     * zero outside the guard": CAS any value > 1 down lock-free, and route
     * only the 1 -> 0 transition through the guarded path below. (A bare
     * fetch_sub that happened to read prev == 1 would have already exposed
     * cnt == 0 before we could take the guard -- the very window the guard
     * is meant to close.)
     */
    for (;;) {
        size_t c = atomic_load_explicit(&wg->cnt, memory_order_acquire);
        if (c == 0) {
            /* Underflow: more done() than add(); mirrors Go's panic. */
            xylem_loge("<waitgroup> done called with zero counter wg=%p",
                       (void*)wg);
            abort();
        }
        if (c == 1) {
            break; /* potential last: take the guarded path */
        }
        if (atomic_compare_exchange_weak_explicit(
                &wg->cnt, &c, c - 1,
                memory_order_acq_rel, memory_order_acquire)) {
            return; /* decremented a value > 1: not last, lock-free */
        }
        /* lost the race; reload and retry */
    }

    /**
     * Counter is 1: decrement it to zero under the guard so the open latch
     * becomes observable only while we hold the guard. A waiter reads cnt
     * only under this same guard, so none can observe the open latch,
     * return, and free wg until we unlock; a thread woken by the broadcast
     * re-takes the guard before it can leave. Then do every remaining
     * access to wg -- drain, open a new generation, broadcast -- here.
     * seq_cst pairs with a thread's armed generation so the wake is never
     * skipped.
     */
    spin_lock(&wg->guard);
    size_t prev = atomic_fetch_sub_explicit(&wg->cnt, 1, memory_order_acq_rel);
    if (prev == 0) {
        spin_unlock(&wg->guard);
        xylem_loge("<waitgroup> done called with zero counter wg=%p",
                   (void*)wg);
        abort();
    }
    if (prev != 1) {
        /* Counter climbed back above 1 between the peek and the guard (a
         * reuse add()); we decremented a value > 1, so we are not last. */
        spin_unlock(&wg->guard);
        return;
    }

    list_swap(&drained, &wg->co_waiters);
    atomic_fetch_add_explicit(&wg->gate, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&wg->thr_waiters, memory_order_seq_cst) > 0) {
        platform_futex_broadcast(&wg->gate);
    }
    spin_unlock(&wg->guard);

    /**
     * Last action: reschedule the drained coroutine waiters. This touches
     * only the privately-owned `drained` list and each waiter's frame
     * (still parked, hence alive, until it resumes), never wg, so a
     * coroutine that resumes and frees wg here cannot strand us -- nothing
     * after this reads wg.
     */
    _wg_wake_all(&drained);
}

void xylem_waitgroup_wait(xylem_waitgroup_t* wg) {
    if (mco_running()) {
        /**
         * Coroutine: park unconditionally; the callback decides under the
         * guard whether to enqueue or run inline (latch already open).
         * There is no unguarded cnt fast path -- reading cnt outside the
         * guard could let this coroutine return and free wg while a
         * concurrent done() is still touching it.
         */
        _wg_co_waiter_t w;
        w.co    = NULL;
        w.sched = runtime_get_scheduler();

        _wg_park_ctx_t ctx = { wg, &w };
        scheduler_park(w.sched, _wg_park_cb, &ctx);
        return;
    }

    /**
     * OS thread: inspect cnt under the guard (the same one done() holds
     * across its critical section), so observing the open latch -- and
     * thus returning to a caller that may destroy wg -- is ordered after
     * done() has finished every access to wg. Snapshot the generation and
     * arm `thr_waiters` under the guard, then release it across the futex
     * sleep; a done() that opens the latch in that window bumps the gate,
     * so the value-compare returns the wait immediately.
     */
    spin_lock(&wg->guard);
    while (atomic_load_explicit(&wg->cnt, memory_order_acquire) != 0) {
        uint32_t g = atomic_load_explicit(&wg->gate, memory_order_relaxed);
        atomic_fetch_add_explicit(&wg->thr_waiters, 1, memory_order_relaxed);
        spin_unlock(&wg->guard);

        platform_futex_wait(&wg->gate, g);

        spin_lock(&wg->guard);
        atomic_fetch_sub_explicit(&wg->thr_waiters, 1, memory_order_relaxed);
    }
    spin_unlock(&wg->guard);
}
