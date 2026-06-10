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

#include "container/list.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"
#include "sync/waiter.h"

#include "runtime/minicoro/minicoro.h"

#include <stdbool.h>
#include <stdlib.h>

/**
 * Cross-context condition variable.
 *
 * Pairs with xylem_mutex; pthread-style edge-triggered semantics (no
 * accumulated signals, so the predicate must be re-checked in a `while`
 * loop). Waiters may be coroutines or OS threads -- both park/block on
 * the FIFO `waiters` list, guarded by a short spin.
 *
 * wait() flow (the lost-wakeup-free ordering):
 *   1. enqueue the caller on the cond's waiters list (under the guard),
 *   2. drop the user mutex,
 *   3. block.
 *
 * The "enqueue before unlock mtx" order is the critical invariant. A
 * signaler serialised through the user mutex cannot observe it released
 * until step 2, by which point the waiter is already linked and visible
 * to signal()/broadcast(). For a coroutine the enqueue + unlock happen
 * inside the park callback, after the coroutine has actually suspended,
 * so a waker can never observe it before it is parked.
 *
 * signal/broadcast just resume waiters; they never park, so they stay
 * callable from any thread or context (the documented cross-thread
 * wakeup pattern: set an atomic predicate, then signal).
 */
struct xylem_cond_s {
    spin_t guard;
    list_t waiters;
};

/* Park callback (runs after the coroutine has suspended): enqueue, then
 * drop the user mutex now that the waiter is linked and visible. */
typedef struct _cond_park_ctx_s {
    xylem_cond_t*  cond;
    xylem_mutex_t* mtx;
    waiter_t*      w;
} _cond_park_ctx_t;

static bool _cond_park_cb(mco_coro* co, void* arg) {
    _cond_park_ctx_t* ctx = (_cond_park_ctx_t*)arg;

    ctx->w->co = co;

    spin_lock(&ctx->cond->guard);
    list_insert_tail(&ctx->cond->waiters, &ctx->w->node);
    spin_unlock(&ctx->cond->guard);

    /* Linked and visible to signalers: now drop the user mutex. */
    xylem_mutex_unlock(ctx->mtx);
    return true;
}

/* Wake every queued waiter in FIFO order. Drains under the guard, then
 * wakes outside it. Reads each waiter's successor before waking it, so a
 * woken thread waiter freeing its stack record cannot strand the drain. */
static void _cond_wake_all(xylem_cond_t* cond) {
    list_t drained;
    list_init(&drained);

    spin_lock(&cond->guard);
    list_swap(&drained, &cond->waiters);
    spin_unlock(&cond->guard);

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

xylem_cond_t* xylem_cond_create(void) {
    xylem_cond_t* c = (xylem_cond_t*)calloc(1, sizeof(xylem_cond_t));
    if (!c) {
        return NULL;
    }
    spin_init(&c->guard);
    list_init(&c->waiters);
    return c;
}

void xylem_cond_destroy(xylem_cond_t* c) {
    if (!c) {
        return;
    }
    free(c);
}

void xylem_cond_wait(xylem_cond_t* cond, xylem_mutex_t* mtx) {
    waiter_t w;

    waiter_init(&w);
    if (w.kind == WAITER_CO) {
        /* Coroutine: park. The park callback enqueues and drops mtx
         * after the yield, so a signaler cannot observe the coroutine
         * before it has actually suspended. */
        _cond_park_ctx_t ctx = { cond, mtx, &w };
        scheduler_park(w.sched, _cond_park_cb, &ctx);
    } else {
        /* External OS thread: block on the per-thread wake sem. */
        spin_lock(&cond->guard);
        if (!w.tsem) {
            /* No wake sem (OOM): cannot block; do not enqueue. */
            spin_unlock(&cond->guard);
        } else {
            list_insert_tail(&cond->waiters, &w.node);
            spin_unlock(&cond->guard);
            /* Linked and visible to signalers: now drop the user mutex. */
            xylem_mutex_unlock(mtx);
            platform_sem_wait(w.tsem);
        }
    }

    /* Woken: restore the "caller holds mtx" postcondition. */
    xylem_mutex_lock(mtx);
}

void xylem_cond_signal(xylem_cond_t* cond) {
    /* Wake the FIFO-oldest waiter, if any. */
    spin_lock(&cond->guard);
    list_node_t* n = list_head(&cond->waiters);
    if (!n) {
        spin_unlock(&cond->guard);
        return;
    }
    list_remove(&cond->waiters, n);

    /* Copy the wake target out before releasing the guard: the waiter's
     * storage may vanish the instant it resumes. */
    waiter_t target = *list_entry(n, waiter_t, node);
    spin_unlock(&cond->guard);

    waiter_wake(target);
}

void xylem_cond_broadcast(xylem_cond_t* cond) {
    _cond_wake_all(cond);
}
