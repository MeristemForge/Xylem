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

#include "container/queue.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"

#include <stdatomic.h>
#include <stdlib.h>

/**
 * Waitgroup concurrency design
 *
 * Counter and waiters list are decoupled:
 *
 *   - `cnt` is a lock-free atomic, manipulated by add/done.
 *   - `waiters` is a FIFO of parked coroutines, protected by a
 *     tiny spin `guard`. One spin section serialises every waiters
 *     mutation so there is no ABA / missed-wakeup window.
 *
 * wait() parks the caller under the guard. Inside the park callback
 * we re-check `cnt == 0`: if done() already drained the list between
 * the fast-path check and the park, we bail out without enqueueing
 * and let the caller run inline.
 *
 * done() fetch-subs cnt. When the previous value was 1, the counter
 * just went to zero and we own the broadcast: swap the waiters list
 * out under the guard and schedule every coroutine in it. When the
 * previous value was 0, the caller called done() more times than
 * add() ever promised -- a hard bug. We log and abort, matching the
 * "negative WaitGroup counter" panic in Go's sync.WaitGroup.
 */

typedef struct _wg_waiter_s {
    queue_node_t node;
    mco_coro*    co;
} _wg_waiter_t;

struct xylem_waitgroup_s {
    atomic_size_t cnt;
    spin_t        guard;
    queue_t       waiters;
};

typedef struct {
    xylem_waitgroup_t* wg;
    _wg_waiter_t       waiter;
} _wg_park_ctx_t;

static bool _wg_park_cb(mco_coro* co, void* arg) {
    _wg_park_ctx_t*    ctx = (_wg_park_ctx_t*)arg;
    xylem_waitgroup_t* wg  = ctx->wg;

    ctx->waiter.co = co;

    spin_lock(&wg->guard);
    /* Re-check under the guard: a done() between the fast-path load
     * and here may have already drained everyone. */
    if (atomic_load_explicit(&wg->cnt, memory_order_acquire) == 0) {
        spin_unlock(&wg->guard);
        return false;
    }
    queue_enqueue(&wg->waiters, &ctx->waiter.node);
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
    queue_init(&wg->waiters);
    return wg;
}

void xylem_waitgroup_destroy(xylem_waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    free(wg);
}

void xylem_waitgroup_add(xylem_waitgroup_t* wg, size_t delta) {
    atomic_fetch_add(&wg->cnt, delta);
}

void xylem_waitgroup_done(xylem_waitgroup_t* wg) {
    size_t prev = atomic_fetch_sub(&wg->cnt, 1);
    if (prev == 0) {
        /* Counter underflowed: done() called more times than add()
         * ever promised. Matches Go's "negative WaitGroup counter"
         * panic. Log first so the diagnostic survives the abort. */
        xylem_loge(
            "<waitgroup> done called with zero counter wg=%p; aborting",
            (void*)wg);
        abort();
    }
    if (prev != 1) {
        return;
    }

    /* Counter just hit zero. Snapshot-and-drain the waiters list
     * under the guard so a racing wait() either enqueues before we
     * take the list (gets woken here) or sees cnt == 0 under the
     * guard and bails without enqueueing. */
    queue_t to_wake;
    queue_init(&to_wake);

    spin_lock(&wg->guard);
    queue_swap(&to_wake, &wg->waiters);
    spin_unlock(&wg->guard);

    scheduler_t* sched = runtime_get_scheduler();
    queue_node_t* n;
    while ((n = queue_dequeue(&to_wake)) != NULL) {
        _wg_waiter_t* w = queue_entry(n, _wg_waiter_t, node);
        scheduler_schedule(sched, w->co);
    }
}

void xylem_waitgroup_wait(xylem_waitgroup_t* wg) {
    if (atomic_load_explicit(&wg->cnt, memory_order_acquire) == 0) {
        return;
    }

    _wg_park_ctx_t ctx;
    ctx.wg = wg;
    scheduler_park(runtime_get_scheduler(), _wg_park_cb, &ctx);
}
