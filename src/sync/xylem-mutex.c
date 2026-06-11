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

#include "xylem/sync/xylem-mutex.h"

#include "container/list.h"
#include "platform/platform-futex.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#define MTX_LOCKED 1u /* the single `state` bit; also the thread futex word */

/**
 * Cross-context coroutine/thread mutex.
 *
 * `state` holds the single LOCKED bit and doubles as the futex word OS
 * threads block on; ownership is that bit, set/cleared via fetch_or.
 *
 * The two waiter kinds block differently, since a coroutine must never
 * block its worker:
 *
 *   - Coroutines park on the scheduler and queue FIFO on `co_waiters`.
 *     unlock hands the lock to the oldest with LOCKED kept set, so
 *     ownership transfers with no kernel round-trip or re-contention --
 *     the common fast path.
 *
 *   - Threads do not queue: they barge (fetch_or) whenever LOCKED is
 *     clear, else sleep in platform_futex_wait. Barging avoids the
 *     wake+block context switch a strict hand-off costs (~us on Windows);
 *     `thr_waiters` only tells unlock whether a wake is needed.
 *
 * `guard` serialises the unlock decision against coroutine enqueue and
 * the LOCKED clear, closing the lost-wakeup race: a coroutine racing
 * unlock is either seen in the list or grabs the freed lock in its park
 * callback, never stranded. Threads need no guard -- futex_wait re-checks
 * the word, so a clear that beats the sleep returns instead of losing it.
 */
struct xylem_mutex_s {
    _Atomic uint32_t state;       /* MTX_LOCKED set = held; thread futex word */
    _Atomic int32_t  thr_waiters; /* OS threads contending on the futex word   */
    spin_t           guard;       /* serialises the coro list + LOCKED clear    */
    list_t           co_waiters;  /* coroutine waiters, FIFO                     */
};

/**
 * A parked coroutine waiter, embedded in the coroutine's lock() frame
 * (which stays alive while parked, so unlock may read it). `granted`
 * tells the woken coroutine which wake it got: 1 = the lock was handed
 * to it (it owns it, return), 0 = a barging release (it must re-contend).
 */
typedef struct _mutex_co_waiter_s {
    list_node_t      node;
    mco_coro*        co;
    scheduler_t*     sched;
    _Atomic uint32_t granted;
} _mutex_co_waiter_t;

typedef struct _mutex_park_ctx_s {
    xylem_mutex_t*      mtx;
    _mutex_co_waiter_t* w;
} _mutex_park_ctx_t;

/* Set LOCKED; the prior value reveals whether it was already held. */
static bool _mutex_try_acquire(xylem_mutex_t* mtx) {
    uint32_t old = atomic_fetch_or_explicit(
        &mtx->state, MTX_LOCKED, memory_order_acquire
    );
    return (old & MTX_LOCKED) == 0;
}

/**
 * Runs after the coroutine has suspended. Under the guard, either grab a
 * lock just freed by a racing unlock (decline the park) or enqueue to be
 * woken -- the ordering that closes the lost-wakeup race.
 */
static bool _mutex_park_cb(mco_coro* co, void* arg) {
    _mutex_park_ctx_t*  ctx = (_mutex_park_ctx_t*)arg;
    xylem_mutex_t*      mtx = ctx->mtx;
    _mutex_co_waiter_t* w   = ctx->w;

    w->co = co;

    spin_lock(&mtx->guard);
    if (_mutex_try_acquire(mtx)) {
        spin_unlock(&mtx->guard);
        atomic_store_explicit(&w->granted, 1, memory_order_release);
        return false; /* acquired in-line: decline the park */
    }
    list_insert_tail(&mtx->co_waiters, &w->node);
    spin_unlock(&mtx->guard);
    return true; /* parked: an unlock will wake us */
}

/**
 * Coroutine slow path. Re-park until woken with the lock handed over
 * (granted) or until a barge succeeds on a release; a lost re-contention
 * loops to re-park.
 */
static void _mutex_lock_co(xylem_mutex_t* mtx) {
    _mutex_co_waiter_t w;
    w.co    = NULL;
    w.sched = runtime_get_scheduler();
    atomic_init(&w.granted, 0);

    _mutex_park_ctx_t ctx = { mtx, &w };
    for (;;) {
        atomic_store_explicit(&w.granted, 0, memory_order_relaxed);
        scheduler_park(w.sched, _mutex_park_cb, &ctx);
        if (atomic_load_explicit(&w.granted, memory_order_acquire)) {
            return; /* handed the lock (or acquired in the callback) */
        }
        if (_mutex_try_acquire(mtx)) {
            return; /* barged in on the released lock */
        }
        /* lost the race; re-park */
    }
}

/**
 * Thread slow path: publish thr_waiters (so unlock can prefer the release
 * path over a coroutine handoff) then barge on the futex word, sleeping
 * while LOCKED is set.
 */
static void _mutex_lock_thr(xylem_mutex_t* mtx) {
    atomic_fetch_add_explicit(&mtx->thr_waiters, 1, memory_order_relaxed);
    while (!_mutex_try_acquire(mtx)) {
        platform_futex_wait(&mtx->state, MTX_LOCKED);
    }
    atomic_fetch_sub_explicit(&mtx->thr_waiters, 1, memory_order_relaxed);
}

/**
 * Detach the FIFO-oldest coroutine waiter, recording in `granted` how it
 * was woken (1 = lock handed over, 0 = re-contend). Reads the waiter out
 * before the caller schedules it, since its frame vanishes once resumed.
 */
static mco_coro* _mutex_take_waiter(
    xylem_mutex_t* mtx, list_node_t* n, uint32_t granted,
    scheduler_t** sched) {
    list_remove(&mtx->co_waiters, n);
    _mutex_co_waiter_t* w  = list_entry(n, _mutex_co_waiter_t, node);
    mco_coro*           co = w->co;
    *sched                 = w->sched;
    atomic_store_explicit(&w->granted, granted, memory_order_release);
    return co;
}

xylem_mutex_t* xylem_mutex_create(void) {
    xylem_mutex_t* mtx = (xylem_mutex_t*)calloc(1, sizeof(xylem_mutex_t));
    if (!mtx) {
        return NULL;
    }
    atomic_init(&mtx->state, 0);
    atomic_init(&mtx->thr_waiters, 0);
    spin_init(&mtx->guard);
    list_init(&mtx->co_waiters);
    return mtx;
}

void xylem_mutex_destroy(xylem_mutex_t* mtx) {
    if (!mtx) {
        return;
    }
    free(mtx);
}

void xylem_mutex_lock(xylem_mutex_t* mtx) {
    /* Uncontended fast path: no park, no block, no guard. */
    if (_mutex_try_acquire(mtx)) {
        return;
    }
    if (mco_running()) {
        _mutex_lock_co(mtx);
    } else {
        _mutex_lock_thr(mtx);
    }
}

bool xylem_mutex_trylock(xylem_mutex_t* mtx) {
    return _mutex_try_acquire(mtx);
}

void xylem_mutex_unlock(xylem_mutex_t* mtx) {
    spin_lock(&mtx->guard);
    list_node_t* n = list_head(&mtx->co_waiters);

    /**
     * Pure coroutine waiters: hand off to the FIFO-oldest with LOCKED
     * left set, so ownership transfers with no free/re-contend. Taken
     * only when no OS thread is contending -- a thread cannot accept a
     * handed lock (it must observe the cleared bit); if thr_waiters is
     * stale-zero here, the thread simply waits until the handed
     * coroutine's own unlock clears and wakes it.
     */
    if (n && atomic_load_explicit(&mtx->thr_waiters,
                                  memory_order_relaxed) == 0) {
        scheduler_t* sched;
        mco_coro*    co = _mutex_take_waiter(mtx, n, 1, &sched);
        spin_unlock(&mtx->guard);
        scheduler_schedule(sched, co);
        return;
    }

    /**
     * Release path: clear LOCKED under the guard so a coroutine enqueuing
     * concurrently is either seen here or grabs the now-free lock in its
     * park callback. Mixed case wakes one of each to re-contend.
     *
     * Wake a blocked thread waiter unconditionally rather than gating on
     * thr_waiters > 0: a waiter that incremented the count but observed
     * LOCKED just before this clear can be missed by the gate, and a
     * two-party rendezvous (a cond ping-pong) has no later unlock to
     * recover the lost wakeup. WakeByAddress / FUTEX_WAKE with no waiter
     * is cheap, so always signalling is the safe default.
     */
    scheduler_t* sched = NULL;
    mco_coro*    co    = n ? _mutex_take_waiter(mtx, n, 0, &sched) : NULL;
    atomic_store_explicit(&mtx->state, 0, memory_order_release);
    spin_unlock(&mtx->guard);

    platform_futex_signal(&mtx->state);
    if (co) {
        scheduler_schedule(sched, co);
    }
}
