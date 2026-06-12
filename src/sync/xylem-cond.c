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
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"
#include "sync/tls-wake.h"

#include "runtime/minicoro/minicoro.h"

#include <stdbool.h>
#include <stdlib.h>

/**
 * Cross-context condition variable.
 *
 * Pairs with xylem_mutex; pthread-style edge-triggered semantics (no
 * accumulated signals, so re-check the predicate in a `while` loop).
 * Coroutine and thread waiters share the FIFO `waiters` list under a
 * short spin; each carries its own wake identity (a coroutine is
 * rescheduled, a thread is released on its per-thread futex word).
 *
 * The lost-wakeup-free invariant is "enqueue before dropping the user
 * mutex": a signaler serialised through that mutex cannot see it released
 * until the waiter is already linked and visible. For a coroutine this
 * happens inside the park callback, after it has actually suspended, so a
 * waker can never observe it pre-park.
 *
 * signal/broadcast only resume waiters and never park, so they are
 * callable from any context (set an atomic predicate, then signal).
 */
struct xylem_cond_s {
    spin_t guard;
    list_t waiters;
};

/* A blocked waiter, embedded in the waiting context's wait() frame. */
typedef enum _cond_kind_e {
    COND_WAITER_CO,
    COND_WAITER_THR,
} _cond_kind_t;

typedef struct _cond_waiter_s {
    list_node_t  node;
    _cond_kind_t kind;
    mco_coro*    co;    /* COND_WAITER_CO  */
    scheduler_t* sched; /* COND_WAITER_CO  */
    tls_wake_t*  wake;  /* COND_WAITER_THR */
} _cond_waiter_t;

/**
 * Wake one waiter by kind: reschedule a coroutine, signal a thread. The
 * argument is a by-value copy taken under the guard, since the waiter's
 * stack frame may vanish the instant it resumes; the thread's wake object
 * is thread-local and outlives the frame, so signalling it stays safe.
 */
static void _cond_wake(_cond_waiter_t w) {
    if (w.kind == COND_WAITER_CO) {
        scheduler_schedule(w.sched, w.co);
    } else {
        tls_wake_signal(w.wake);
    }
}

/* Pop and snapshot the FIFO-oldest waiter; caller holds the guard. */
static bool _cond_pop(list_t* waiters, _cond_waiter_t* out) {
    list_node_t* n = list_head(waiters);
    if (!n) {
        return false;
    }
    list_remove(waiters, n);
    *out = *list_entry(n, _cond_waiter_t, node);
    return true;
}

/**
 * Wake every waiter in a drained, privately-owned list, in FIFO order.
 * Reads each node's successor before waking it, so a thread waiter that
 * frees its frame on resume cannot strand the walk.
 */
static void _cond_wake_all(list_t* drained) {
    list_node_t* sentinel = list_sentinel(drained);
    list_node_t* n        = list_head(drained);
    while (n) {
        list_node_t* next = list_next(n);
        if (next == sentinel) {
            next = NULL;
        }
        _cond_wake(*list_entry(n, _cond_waiter_t, node));
        n = next;
    }
}

typedef struct _cond_park_ctx_s {
    xylem_cond_t*   cond;
    xylem_mutex_t*  mtx;
    _cond_waiter_t* w;
} _cond_park_ctx_t;

/**
 * Runs after the coroutine suspends: enqueue, then drop the user mutex
 * now that the waiter is linked and visible to signalers.
 */
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
    _cond_waiter_t w;

    if (mco_running()) {
        /* Coroutine: park; the callback enqueues and drops mtx post-suspend. */
        w.kind  = COND_WAITER_CO;
        w.co    = NULL;
        w.sched = runtime_get_scheduler();

        _cond_park_ctx_t ctx = { cond, mtx, &w };
        scheduler_park(w.sched, _cond_park_cb, &ctx);
    } else {
        /* External OS thread: block on the per-thread futex wake. */
        w.kind = COND_WAITER_THR;
        w.wake = tls_wake_self();

        spin_lock(&cond->guard);
        if (!w.wake) {
            /* No wake object (OOM): cannot block; do not enqueue. */
            spin_unlock(&cond->guard);
        } else {
            list_insert_tail(&cond->waiters, &w.node);
            spin_unlock(&cond->guard);
            /* Linked and visible to signalers: now drop the user mutex. */
            xylem_mutex_unlock(mtx);
            tls_wake_wait(w.wake);
        }
    }

    /* Woken: restore the "caller holds mtx" postcondition. */
    xylem_mutex_lock(mtx);
}

void xylem_cond_signal(xylem_cond_t* cond) {
    spin_lock(&cond->guard);
    _cond_waiter_t target;
    bool           got = _cond_pop(&cond->waiters, &target);
    spin_unlock(&cond->guard);

    if (got) {
        _cond_wake(target);
    }
}

void xylem_cond_broadcast(xylem_cond_t* cond) {
    list_t drained;
    list_init(&drained);

    spin_lock(&cond->guard);
    list_swap(&drained, &cond->waiters);
    spin_unlock(&cond->guard);

    _cond_wake_all(&drained);
}
