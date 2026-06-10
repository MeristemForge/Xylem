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
#include "runtime/scheduler.h"
#include "sync/spin.h"
#include "sync/waiter.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdlib.h>

/**
 * Cross-context coroutine/thread mutex.
 *
 * Ownership is held between lock() and unlock() by whoever acquired it
 * -- a coroutine or a plain OS thread. A contended lock() parks the
 * coroutine on the scheduler, or blocks the OS thread on its per-thread
 * wake sem; both kinds queue FIFO on `waiters`, guarded by a short spin.
 *
 * Direct hand-off: unlock() pops the FIFO-oldest waiter and wakes it
 * *without* clearing `state`, so the woken party returns from lock()
 * already owning the mutex. Only an unlock that finds no waiter clears
 * `state` back to 0. This is what makes ownership independent of the
 * acquiring/releasing thread.
 */
struct xylem_mutex_s {
    _Atomic uint32_t state; /* 0 = free, 1 = held */
    spin_t           guard; /* serialises the waiters list           */
    list_t           waiters;
};

/* Try to grab a free lock under the guard. On a hand-off wake `state`
 * stays 1, so a woken waiter does not run this and already owns the
 * lock. */
static bool _mutex_try_acquire(xylem_mutex_t* mtx) {
    uint32_t expected = 0;
    return atomic_compare_exchange_strong(&mtx->state, &expected, 1);
}

/* Park callback (runs after the coroutine has suspended). Re-checks the
 * lock under the guard so an unlock racing the block either hands the
 * lock over (CAS wins -> decline the park, run inline) or finds us
 * queued for a later hand-off. */
typedef struct _mutex_park_ctx_s {
    xylem_mutex_t* mtx;
    waiter_t*      w;
} _mutex_park_ctx_t;

static bool _mutex_park_cb(mco_coro* co, void* arg) {
    _mutex_park_ctx_t* ctx = (_mutex_park_ctx_t*)arg;
    xylem_mutex_t*     mtx = ctx->mtx;

    ctx->w->co = co;

    spin_lock(&mtx->guard);
    if (_mutex_try_acquire(mtx)) {
        spin_unlock(&mtx->guard);
        return false; /* acquired: decline the park, run inline */
    }
    list_insert_tail(&mtx->waiters, &ctx->w->node);
    spin_unlock(&mtx->guard);
    return true;
}

xylem_mutex_t* xylem_mutex_create(void) {
    xylem_mutex_t* mtx = (xylem_mutex_t*)calloc(1, sizeof(xylem_mutex_t));
    if (!mtx) {
        return NULL;
    }
    atomic_init(&mtx->state, 0);
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
    /* Uncontended fast path: no park/block, no guard. */
    if (_mutex_try_acquire(mtx)) {
        return;
    }

    /* Contended: block. The guard re-checks the lock so an unlock racing
     * the block either hands the lock over or queues us for a hand-off. */
    waiter_t w;

    waiter_init(&w);
    if (w.kind == WAITER_CO) {
        _mutex_park_ctx_t ctx = { mtx, &w };
        scheduler_park(w.sched, _mutex_park_cb, &ctx);
        return;
    }

    /* External OS thread: block on the per-thread wake sem. */
    spin_lock(&mtx->guard);
    if (_mutex_try_acquire(mtx)) {
        spin_unlock(&mtx->guard);
        return;
    }
    if (!w.tsem) {
        /* No wake sem (OOM): a waker would deref a NULL tsem and we could
         * never be resumed, so do not enqueue. Best-effort on a
         * catastrophic process-lifetime allocation failure. */
        spin_unlock(&mtx->guard);
        return;
    }
    list_insert_tail(&mtx->waiters, &w.node);
    spin_unlock(&mtx->guard);
    platform_sem_wait(w.tsem);
}

bool xylem_mutex_trylock(xylem_mutex_t* mtx) {
    return _mutex_try_acquire(mtx);
}

void xylem_mutex_unlock(xylem_mutex_t* mtx) {
    /* Hand the lock to the FIFO-oldest waiter (leaving state == 1), or
     * clear state when there is none -- both under the guard so a lock()
     * that fails its CAS cannot enqueue between the empty check and the
     * clear and be stranded (a lost wakeup). */
    spin_lock(&mtx->guard);
    list_node_t* n = list_head(&mtx->waiters);
    if (!n) {
        atomic_store(&mtx->state, 0);
        spin_unlock(&mtx->guard);
        return;
    }
    list_remove(&mtx->waiters, n);

    /* Copy the wake target out before releasing the guard: the waiter's
     * storage may vanish the instant it resumes. */
    waiter_t target = *list_entry(n, waiter_t, node);
    spin_unlock(&mtx->guard);

    waiter_wake(target);
}
