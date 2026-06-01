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

#include "xylem/sync/xylem-cond.h"

#include "xylem/sync/xylem-mutex.h"
#include "xylem/xylem-logger.h"

#include "container/queue.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"

#include "runtime/minicoro/minicoro.h"

#include <stdbool.h>
#include <stdlib.h>

/**
 * Condition variable design
 *
 * State:
 *   - `guard` protects the `waiters` list. Every list mutation
 *     (enqueue in wait, dequeue in signal, swap in broadcast)
 *     happens under the spin so there is no ABA or missed-wakeup
 *     window internal to the cond itself.
 *
 * wait() flow:
 *   1. scheduler_park yields the coroutine and then invokes the
 *      park callback on the worker.
 *   2. The park callback enqueues the waiter on `waiters` under
 *      the guard, then releases the user mutex.
 *
 *   The "enqueue before release mtx" order is the critical
 *   invariant. A racing signaler can only see the released mutex
 *   by taking it (the standard `mtx_lock + modify state + signal`
 *   idiom); by the time it succeeds, the enqueue has already
 *   completed, so signal/broadcast will find this waiter.
 *
 *   Reversing the two (unlock first, enqueue second) reintroduces
 *   the classic pthread_cond lost-wakeup bug.
 *
 * signal/broadcast flow:
 *   Snapshot-and-schedule under `guard`. scheduler_schedule is
 *   called outside the guard: the schedule path may touch
 *   scheduler state and we must not hold a spin across any
 *   potentially long-running operation.
 */

typedef struct _cond_waiter_s {
    queue_node_t node;
    mco_coro*    co;
} _cond_waiter_t;

struct xylem_cond_s {
    spin_t  guard;
    queue_t waiters;
};

typedef struct {
    xylem_cond_t*  cond;
    xylem_mutex_t* mtx;
    _cond_waiter_t waiter;
} _cond_park_ctx_t;

static bool _cond_park_cb(mco_coro* co, void* arg) {
    _cond_park_ctx_t* ctx = (_cond_park_ctx_t*)arg;
    ctx->waiter.co        = co;

    /**
     * Link the waiter first, then drop the user mutex. A signaler
     * serialised through `mtx` cannot observe the unlocked state
     * until this function returns from xylem_mutex_unlock, and by
     * that point the waiter is already on the list.
     */
    spin_lock(&ctx->cond->guard);
    queue_enqueue(&ctx->cond->waiters, &ctx->waiter.node);
    spin_unlock(&ctx->cond->guard);

    xylem_mutex_unlock(ctx->mtx);
    return true;
}

xylem_cond_t* xylem_cond_create(void) {
    xylem_cond_t* c = (xylem_cond_t*)calloc(1, sizeof(xylem_cond_t));
    if (!c) {
        return NULL;
    }
    spin_init(&c->guard);
    queue_init(&c->waiters);
    return c;
}

void xylem_cond_destroy(xylem_cond_t* c) {
    if (!c) {
        return;
    }
    free(c);
}

void xylem_cond_wait(xylem_cond_t* cond, xylem_mutex_t* mtx) {
    /**
     * wait() must park the current coroutine. Outside a coroutine
     * context scheduler_park would abort on its own; catch it here
     * for a diagnostic that names the misused API.
     */
    if (!mco_running()) {
        xylem_loge(
            "xylem_cond_wait called without a coroutine context "
            "(cond=%p); cond wait is coroutine-owned and must be "
            "called from inside a coroutine on a scheduler worker; "
            "aborting",
            (void*)cond);
        abort();
    }

    _cond_park_ctx_t ctx;
    ctx.cond = cond;
    ctx.mtx  = mtx;
    scheduler_park(runtime_get_scheduler(), _cond_park_cb, &ctx);

    /**
     * Woken. Restore the "caller holds mtx" postcondition. Note
     * that xylem_mutex_lock requires a coroutine context, which we
     * still have here: we returned from scheduler_park inside the
     * same coroutine.
     */
    xylem_mutex_lock(mtx);
}

void xylem_cond_signal(xylem_cond_t* cond) {
    spin_lock(&cond->guard);
    queue_node_t* n = queue_dequeue(&cond->waiters);
    spin_unlock(&cond->guard);

    if (n) {
        _cond_waiter_t* w = queue_entry(n, _cond_waiter_t, node);
        scheduler_schedule(runtime_get_scheduler(), w->co);
    }
}

void xylem_cond_broadcast(xylem_cond_t* cond) {
    queue_t to_wake;
    queue_init(&to_wake);

    spin_lock(&cond->guard);
    queue_swap(&to_wake, &cond->waiters);
    spin_unlock(&cond->guard);

    scheduler_t*  sched = runtime_get_scheduler();
    queue_node_t* n;
    while ((n = queue_dequeue(&to_wake)) != NULL) {
        _cond_waiter_t* w = queue_entry(n, _cond_waiter_t, node);
        scheduler_schedule(sched, w->co);
    }
}
