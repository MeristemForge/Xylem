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

/**
 * `state` is a classic 3-state futex word, and also the word OS threads
 * block on:
 *
 *   FREE      0  unlocked, no waiter recorded
 *   LOCKED    1  held, no OS thread known to be blocked
 *   CONTENDED 2  held, an OS thread may be blocked in platform_futex_wait
 *
 * The CONTENDED value is what lets unlock avoid a wake syscall when none
 * is needed: an OS thread publishes it (xchg -> 2) before it sleeps, so a
 * release that observes anything other than 2 is provably free of blocked
 * threads and skips the wake. A thread that acquires from a contended
 * state re-publishes 2 (it cannot know whether others still wait), so the
 * flag is self-correcting -- the next clean FREE->LOCKED acquire drops it.
 */
#define MTX_FREE      0u
#define MTX_LOCKED    1u
#define MTX_CONTENDED 2u

/**
 * Cross-context coroutine/thread mutex.
 *
 * The two waiter kinds block differently, since a coroutine must never
 * block its worker:
 *
 *   - Coroutines park on the scheduler and queue FIFO on `co_waiters`.
 *     unlock hands the lock to the oldest with the word kept held, so
 *     ownership transfers with no kernel round-trip or re-contention --
 *     the common fast path. Coroutines never touch CONTENDED; they only
 *     ever acquire cleanly (FREE -> LOCKED).
 *
 *   - Threads do not queue: they run the 3-state futex protocol on
 *     `state`, publishing CONTENDED before sleeping and barging on every
 *     wake. The word itself records whether a wake is owed, so unlock
 *     needs no separate waiter counter.
 *
 * `guard` serialises the unlock decision against coroutine enqueue and
 * the lock clear, closing the lost-wakeup race: a coroutine racing unlock
 * is either seen in the list or grabs the freed lock in its park
 * callback, never stranded. Threads need no guard -- futex_wait re-checks
 * the word, so a clear that beats the sleep returns instead of losing it,
 * and the release reads the authoritative word value (via exchange) to
 * decide the wake.
 */
struct xylem_mutex_s {
    _Atomic uint32_t state;      /* 3-state futex word (see above)          */
    spin_t           guard;      /* serialises the coro list + lock clear   */
    list_t           co_waiters; /* coroutine waiters, FIFO                 */
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

/* Acquire only from FREE: clean CAS FREE -> LOCKED, valid in any context. */
static bool _mutex_try_acquire(xylem_mutex_t* mtx) {
    uint32_t expected = MTX_FREE;
    return atomic_compare_exchange_strong_explicit(
        &mtx->state, &expected, MTX_LOCKED,
        memory_order_acquire, memory_order_relaxed
    );
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
 * Thread slow path: the classic 3-state futex acquire. Try a clean
 * FREE -> LOCKED once more (the lock may have freed since the fast path),
 * else publish CONTENDED and sleep while the word still holds it, barging
 * on every wake. Acquiring from contention re-stamps CONTENDED so a later
 * unlock still wakes any remaining sleeper.
 */
static void _mutex_lock_thr(xylem_mutex_t* mtx) {
    uint32_t c = MTX_FREE;
    if (atomic_compare_exchange_strong_explicit(
            &mtx->state, &c, MTX_LOCKED,
            memory_order_acquire, memory_order_relaxed)) {
        return;
    }
    if (c != MTX_CONTENDED) {
        c = atomic_exchange_explicit(
            &mtx->state, MTX_CONTENDED, memory_order_acquire);
    }
    while (c != MTX_FREE) {
        platform_futex_wait(&mtx->state, MTX_CONTENDED);
        c = atomic_exchange_explicit(
            &mtx->state, MTX_CONTENDED, memory_order_acquire);
    }
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
    atomic_init(&mtx->state, MTX_FREE);
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
     * Pure coroutine hand-off: give the lock to the FIFO-oldest with the
     * word kept held, so ownership transfers with no free/re-contend.
     * Taken only when no OS thread is blocked (state != CONTENDED) -- a
     * thread cannot accept a handed lock, it must observe the cleared
     * word. The read may race a thread stamping CONTENDED right after;
     * that is safe, because we do not clear here, so the flag persists
     * and the handed coroutine's own unlock wakes the thread.
     */
    if (n && atomic_load_explicit(&mtx->state, memory_order_acquire)
                != MTX_CONTENDED) {
        scheduler_t* sched;
        mco_coro*    co = _mutex_take_waiter(mtx, n, 1, &sched);
        spin_unlock(&mtx->guard);
        scheduler_schedule(sched, co);
        return;
    }

    /**
     * Release path: clear the lock under the guard (so a coroutine
     * enqueuing concurrently is either seen above or grabs the freed lock
     * in its park callback) and read the word's authoritative prior value
     * via the exchange. A blocked thread is woken exactly when that value
     * was CONTENDED -- no separate counter, no unconditional syscall. A
     * mixed wait wakes one of each to re-contend.
     */
    scheduler_t* sched = NULL;
    mco_coro*    co    = n ? _mutex_take_waiter(mtx, n, 0, &sched) : NULL;
    uint32_t     prev  = atomic_exchange_explicit(
        &mtx->state, MTX_FREE, memory_order_release);
    spin_unlock(&mtx->guard);

    if (prev == MTX_CONTENDED) {
        platform_futex_signal(&mtx->state);
    }
    if (co) {
        scheduler_schedule(sched, co);
    }
}
