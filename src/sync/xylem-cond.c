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
#include "sync/thrd-wake.h"

#include "runtime/minicoro/minicoro.h"

#include <stdbool.h>
#include <stdint.h>
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
    spin_t       guard;
    list_t       waiters;
    scheduler_t* sched;
};

typedef enum _waiter_kind_e {
    WAITER_CORO,
    WAITER_THRD,
} _waiter_kind_t;

typedef struct _waiter_s {
    list_node_t    node;
    _waiter_kind_t kind;
    xylem_cond_t*  cond;
    xylem_mutex_t* mtx;
} _waiter_t;

typedef struct _coro_waiter_s {
    _waiter_t base;
    mco_coro* co;
} _coro_waiter_t;

typedef struct _thrd_waiter_s {
    _waiter_t    base;
    thrd_wake_t* wake;
} _thrd_waiter_t;

static void _cond_consume_credit(uint32_t cost) {
    if (runtime_consume_credit(cost)) {
        runtime_yield_credit();
    }
}

static void _cond_push_waiter(list_t* waiters, _waiter_t* w) {
    list_insert_tail(waiters, &w->node);
}

static _waiter_t* _cond_pop_waiter(list_t* waiters) {
    list_node_t* n = list_head(waiters);
    if (!n) {
        return NULL;
    }
    _waiter_t* w = list_entry(n, _waiter_t, node);
    list_remove(waiters, &w->node);
    return w;
}

static void _cond_wake(xylem_cond_t* cond, _waiter_t* w) {
    if (w->kind == WAITER_CORO) {
        _coro_waiter_t* cw = list_entry(w, _coro_waiter_t, base);
        scheduler_schedule(cond->sched, cw->co);
    } else {
        _thrd_waiter_t* tw = list_entry(w, _thrd_waiter_t, base);
        thrd_wake_signal(tw->wake);
    }
}

static void _cond_wake_all(xylem_cond_t* cond, list_t* wake_list) {
    for (;;) {
        _waiter_t* w = _cond_pop_waiter(wake_list);
        if (!w) {
            return;
        }
        _cond_wake(cond, w);
    }
}

/**
 * Runs after the coroutine suspends: enqueue, then drop the user mutex
 * now that the waiter is linked and visible to signalers.
 */
static bool _cond_park_cb(mco_coro* co, void* arg) {
    _coro_waiter_t* w    = (_coro_waiter_t*)arg;
    xylem_cond_t*   cond = w->base.cond;

    w->co = co;

    spin_lock(&cond->guard);
    _cond_push_waiter(&cond->waiters, &w->base);
    spin_unlock(&cond->guard);

    xylem_mutex_unlock(w->base.mtx);
    return true;
}

static void _cond_wait_coro(xylem_cond_t* cond, xylem_mutex_t* mtx) {
    _coro_waiter_t w;
    w.base.kind = WAITER_CORO;
    w.base.cond = cond;
    w.base.mtx  = mtx;
    w.co        = NULL;

    scheduler_park(cond->sched, _cond_park_cb, &w);
}

static void _cond_wait_thrd(xylem_cond_t* cond, xylem_mutex_t* mtx) {
    _thrd_waiter_t w;
    w.base.kind = WAITER_THRD;
    w.base.cond = cond;
    w.base.mtx  = mtx;
    w.wake      = thrd_wake_self();

    spin_lock(&cond->guard);
    _cond_push_waiter(&cond->waiters, &w.base);
    spin_unlock(&cond->guard);

    xylem_mutex_unlock(mtx);
    thrd_wake_wait(w.wake);
}

xylem_cond_t* xylem_cond_create(void) {
    xylem_cond_t* c = (xylem_cond_t*)calloc(1, sizeof(xylem_cond_t));
    if (!c) {
        return NULL;
    }
    spin_init(&c->guard);
    list_init(&c->waiters);
    c->sched = runtime_get_scheduler();
    return c;
}

void xylem_cond_destroy(xylem_cond_t* c) {
    if (!c) {
        return;
    }
    free(c);
}

void xylem_cond_wait(xylem_cond_t* cond, xylem_mutex_t* mtx) {
    if (mco_running()) {
        _cond_wait_coro(cond, mtx);
    } else {
        _cond_wait_thrd(cond, mtx);
    }

    xylem_mutex_lock(mtx);
}

void xylem_cond_signal(xylem_cond_t* cond) {
    spin_lock(&cond->guard);
    _waiter_t* target = _cond_pop_waiter(&cond->waiters);
    spin_unlock(&cond->guard);

    if (target) {
        _cond_wake(cond, target);
    }
    _cond_consume_credit(1);
}

void xylem_cond_broadcast(xylem_cond_t* cond) {
    list_t drained;
    list_init(&drained);

    spin_lock(&cond->guard);
    list_swap(&drained, &cond->waiters);
    spin_unlock(&cond->guard);

    if (!list_empty(&drained)) {
        _cond_wake_all(cond, &drained);
    }
    _cond_consume_credit(1);
}
