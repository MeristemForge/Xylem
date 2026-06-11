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
 * `state` carries the one LOCKED bit and doubles as the futex word OS
 * threads block on. Ownership is taken by atomically setting LOCKED
 * (fetch_or) and dropped by clearing it; it is held between lock() and
 * unlock() by whoever acquired it, coroutine or plain OS thread.
 *
 * The two waiter kinds block differently, because a coroutine must never
 * block its worker:
 *
 *   - Coroutines park on the scheduler and queue FIFO on `waiters` (under
 *     `guard`). unlock hands the lock straight to the oldest -- keeping
 *     LOCKED set so ownership transfers with no kernel round-trip and no
 *     re-contention. This is the common, fair, fast path for the runtime.
 *
 *   - OS threads do not queue: they spin the futex word and acquire by
 *     barging (fetch_or) whenever LOCKED is clear, sleeping in
 *     platform_futex_wait otherwise. unlock clears LOCKED and wakes one.
 *     Barging skips the forced wake+block context switch that a strict
 *     hand-off would cost a thread (~us on Windows).
 *
 * Mixed contention (threads AND coroutines on one lock, the rare case)
 * stays correct, not optimal: unlock then takes the release path, clears
 * LOCKED, wakes one thread, and reschedules one coroutine to re-contend.
 *
 * `guard` serialises the unlock decision (hand-off vs release) against
 * coroutine enqueue and the LOCKED clear, which is what makes the design
 * free of lost wakeups: a coroutine racing an unlock either is seen in
 * the list under the guard, or runs its park callback after LOCKED is
 * cleared and acquires the free lock there -- never stranded. Threads
 * need no such guarding: platform_futex_wait re-checks the word, so an
 * unlock that clears LOCKED before the thread sleeps makes the wait
 * return at once instead of losing the wakeup.
 */
struct xylem_mutex_s {
    _Atomic uint32_t state;       /* MTX_LOCKED set = held; thread futex word */
    _Atomic int32_t  thr_waiters; /* OS threads contending on the futex word   */
    spin_t           guard;       /* serialises the coro list + LOCKED clear    */
    list_t           waiters;     /* coroutine waiters, FIFO                     */
};

/* A parked coroutine waiter, embedded in the coroutine's lock() frame
 * (which stays alive while parked, so unlock may read it). `granted`
 * tells the woken coroutine which wake it got: 1 = the lock was handed
 * to it (it owns it, return), 0 = a barging release (it must re-contend). */
typedef struct _mutex_coro_waiter_s {
    list_node_t      node;
    mco_coro*        co;
    scheduler_t*     sched;
    _Atomic uint32_t granted;
} _mutex_coro_waiter_t;

typedef struct _mutex_park_ctx_s {
    xylem_mutex_t*        mtx;
    _mutex_coro_waiter_t* w;
} _mutex_park_ctx_t;

/* Acquire by setting LOCKED. fetch_or preserves any other state bits and
 * tells us, from the old value, whether LOCKED was already set (someone
 * else holds it). */
static bool _mutex_try_acquire(xylem_mutex_t* mtx) {
    uint32_t old = atomic_fetch_or_explicit(
        &mtx->state, MTX_LOCKED, memory_order_acquire
    );
    return (old & MTX_LOCKED) == 0;
}

/* Park callback (runs after the coroutine has suspended). Re-checks the
 * lock under the guard: an unlock racing the park either already cleared
 * LOCKED (we acquire here and decline the park) or will find us queued. */
static bool _mutex_park_cb(mco_coro* co, void* arg) {
    _mutex_park_ctx_t*    ctx = (_mutex_park_ctx_t*)arg;
    xylem_mutex_t*        mtx = ctx->mtx;
    _mutex_coro_waiter_t* w   = ctx->w;

    w->co = co;

    spin_lock(&mtx->guard);
    if (_mutex_try_acquire(mtx)) {
        spin_unlock(&mtx->guard);
        atomic_store_explicit(&w->granted, 1, memory_order_release);
        return false; /* acquired: decline park, resume owning the lock */
    }
    list_insert_tail(&mtx->waiters, &w->node);
    spin_unlock(&mtx->guard);
    return true; /* parked: an unlock will wake us */
}

/* Coroutine slow path: park, then act on how we were woken. A hand-off
 * (granted) means we own the lock; a barging release means re-contend,
 * re-parking on a lost race. */
static void _mutex_lock_coro(xylem_mutex_t* mtx) {
    _mutex_coro_waiter_t w;
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
        /* Lost the re-contention; loop re-parks. */
    }
}

/* OS-thread slow path: barge on the futex word, sleeping while held.
 * thr_waiters is published before sleeping so unlock knows to wake us. */
static void _mutex_lock_thread(xylem_mutex_t* mtx) {
    atomic_fetch_add_explicit(&mtx->thr_waiters, 1, memory_order_relaxed);
    while (!_mutex_try_acquire(mtx)) {
        platform_futex_wait(&mtx->state, MTX_LOCKED);
    }
    atomic_fetch_sub_explicit(&mtx->thr_waiters, 1, memory_order_relaxed);
}

xylem_mutex_t* xylem_mutex_create(void) {
    xylem_mutex_t* mtx = (xylem_mutex_t*)calloc(1, sizeof(xylem_mutex_t));
    if (!mtx) {
        return NULL;
    }
    atomic_init(&mtx->state, 0);
    atomic_init(&mtx->thr_waiters, 0);
    spin_init(&mtx->guard);
    list_init(&mtx->waiters);
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
        _mutex_lock_coro(mtx);
    } else {
        _mutex_lock_thread(mtx);
    }
}

bool xylem_mutex_trylock(xylem_mutex_t* mtx) {
    return _mutex_try_acquire(mtx);
}

void xylem_mutex_unlock(xylem_mutex_t* mtx) {
    spin_lock(&mtx->guard);
    list_node_t* n   = list_head(&mtx->waiters);
    bool         thr = atomic_load_explicit(&mtx->thr_waiters, memory_order_relaxed) > 0;

    if (n && !thr) {
        /* Pure coroutine waiters: hand off to the FIFO-oldest, leaving
         * LOCKED set so ownership transfers with no free/re-contend. */
        list_remove(&mtx->waiters, n);
        _mutex_coro_waiter_t* w     = list_entry(n, _mutex_coro_waiter_t, node);
        mco_coro*             co    = w->co;
        scheduler_t*          sched = w->sched;
        atomic_store_explicit(&w->granted, 1, memory_order_release);
        spin_unlock(&mtx->guard);
        scheduler_schedule(sched, co);
        return;
    }

    /* Release path: clear LOCKED under the guard so a coroutine enqueuing
     * concurrently is either seen here or acquires the now-free lock in
     * its park callback. In the mixed case (a coroutine queued while
     * threads also wait) wake one of each to re-contend. */
    mco_coro*    co    = NULL;
    scheduler_t* sched = NULL;
    if (n) {
        list_remove(&mtx->waiters, n);
        _mutex_coro_waiter_t* w = list_entry(n, _mutex_coro_waiter_t, node);
        co    = w->co;
        sched = w->sched;
        atomic_store_explicit(&w->granted, 0, memory_order_release);
    }
    atomic_store_explicit(&mtx->state, 0, memory_order_release);
    spin_unlock(&mtx->guard);

    if (thr) {
        platform_futex_signal(&mtx->state);
    }
    if (co) {
        scheduler_schedule(sched, co);
    }
}
