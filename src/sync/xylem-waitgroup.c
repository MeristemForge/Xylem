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
#include "runtime/scheduler.h"
#include "sync/spin.h"
#include "sync/waiter.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdlib.h>

/**
 * Cross-context countdown latch (Go's sync.WaitGroup shape).
 *
 * Counter and waiter queue are decoupled:
 *   - `cnt` is a lock-free atomic bumped by add()/done().
 *   - `waiters` is a FIFO of parked coroutines / blocked OS threads,
 *     guarded by a short spin.
 *
 * wait() parks the caller (coroutine) or blocks it (OS thread). The
 * block decision re-checks `cnt == 0` under the guard, so a done() that
 * drained the list between the fast-path load and the block makes the
 * caller run inline instead of stranding.
 *
 * done() fetch-subs cnt; on the 1 -> 0 transition it owns the broadcast
 * and wakes every waiter. cnt is written before done() takes the guard
 * to drain, and wait() reads cnt under that same guard, so the two
 * cannot miss each other:
 *   - wait() takes the guard first -> sees cnt > 0, enqueues; done()
 *     then drains and wakes it.
 *   - done() takes the guard first -> wakes nothing; wait() then sees
 *     cnt == 0 and declines to block.
 */
struct xylem_waitgroup_s {
    atomic_size_t cnt;
    spin_t        guard;
    list_t        waiters;
};

/* Park callback (runs after the coroutine has suspended): re-check the
 * latch under the guard, enqueue only if still closed. */
typedef struct _wg_park_ctx_s {
    xylem_waitgroup_t* wg;
    waiter_t*          w;
} _wg_park_ctx_t;

static bool _wg_park_cb(mco_coro* co, void* arg) {
    _wg_park_ctx_t*    ctx = (_wg_park_ctx_t*)arg;
    xylem_waitgroup_t* wg  = ctx->wg;

    ctx->w->co = co;

    spin_lock(&wg->guard);
    if (atomic_load_explicit(&wg->cnt, memory_order_acquire) == 0) {
        /* Latch already open: decline the park, run inline. */
        spin_unlock(&wg->guard);
        return false;
    }
    list_insert_tail(&wg->waiters, &ctx->w->node);
    spin_unlock(&wg->guard);
    return true;
}

xylem_waitgroup_t* xylem_waitgroup_create(void) {
    xylem_waitgroup_t* wg =
        (xylem_waitgroup_t*)calloc(1, sizeof(xylem_waitgroup_t));
    if (!wg) {
        return NULL;
    }
    atomic_init(&wg->cnt, 0);
    spin_init(&wg->guard);
    list_init(&wg->waiters);
    return wg;
}

void xylem_waitgroup_destroy(xylem_waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    free(wg);
}

void xylem_waitgroup_add(xylem_waitgroup_t* wg, size_t delta) {
    /* Lock-free bump; neither parks nor wakes, so any-thread/any-context. */
    atomic_fetch_add(&wg->cnt, delta);
}

void xylem_waitgroup_done(xylem_waitgroup_t* wg) {
    size_t prev = atomic_fetch_sub(&wg->cnt, 1);
    if (prev == 0) {
        /* Underflow: done() called more than add() ever promised. Matches
         * Go's "negative WaitGroup counter" panic. Log before aborting. */
        xylem_loge(
            "<waitgroup> done called with zero counter wg=%p; aborting",
            (void*)wg);
        abort();
    }
    if (prev != 1) {
        return;
    }

    /* Counter just hit zero: release everyone, FIFO. Drain under the
     * guard, then wake outside it. Read each waiter's successor before
     * waking it, so a woken thread waiter freeing its stack record
     * cannot strand the drain. */
    list_t drained;
    list_init(&drained);

    spin_lock(&wg->guard);
    list_swap(&drained, &wg->waiters);
    spin_unlock(&wg->guard);

    list_node_t* sentinel = list_sentinel(&drained);
    list_node_t* n        = list_head(&drained);
    while (n) {
        list_node_t* next = list_next(n);
        if (next == sentinel) {
            next = NULL;
        }
        waiter_wake(*list_entry(n, waiter_t, node));
        n = next;
    }
}

void xylem_waitgroup_wait(xylem_waitgroup_t* wg) {
    /* Fast path: already open, no block. */
    if (atomic_load_explicit(&wg->cnt, memory_order_acquire) == 0) {
        return;
    }

    waiter_t w;

    waiter_init(&w);
    if (w.kind == WAITER_CO) {
        /* Coroutine: park. The park callback re-checks the counter under
         * the guard after the yield, closing the race with done(). */
        _wg_park_ctx_t ctx = { wg, &w };
        scheduler_park(w.sched, _wg_park_cb, &ctx);
        return;
    }

    /* External OS thread: block on the per-thread wake sem. */
    spin_lock(&wg->guard);
    if (atomic_load_explicit(&wg->cnt, memory_order_acquire) == 0) {
        spin_unlock(&wg->guard);
        return;
    }
    if (!w.tsem) {
        /* No wake sem (OOM): cannot block; do not enqueue. */
        spin_unlock(&wg->guard);
        return;
    }
    list_insert_tail(&wg->waiters, &w.node);
    spin_unlock(&wg->guard);
    platform_sem_wait(w.tsem);
}
