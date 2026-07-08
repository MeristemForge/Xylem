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
#include "platform/platform-cpu.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"
#include "sync/thrd-wake.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define MUTEX_SPIN_TRIES 128u

/**
 * Cross-context coroutine/thread mutex.
 *
 * Coroutine and OS-thread waiters share one sleep queue. unlock() releases
 * the lock before waking one waiter, so resumed waiters re-contend instead
 * of receiving ownership by handoff.
 */
struct xylem_mutex_s {
    _Atomic bool locked;
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

static bool _mutex_try_take(xylem_mutex_t* mtx) {
    bool expected = false;
    return atomic_compare_exchange_strong(&mtx->locked, &expected, true);
}

static bool _mutex_spin_take(xylem_mutex_t* mtx) {
    for (uint32_t i = 0; i < MUTEX_SPIN_TRIES; i++) {
        platform_cpu_relax();
        if (_mutex_try_take(mtx)) {
            return true;
        }
    }
    return false;
}

static void _mutex_push_waiter(list_t* waiters, _waiter_t* w) {
    list_insert_tail(waiters, &w->node);
}

static _waiter_t* _mutex_pop_waiter(list_t* waiters) {
    list_node_t* n = list_head(waiters);
    if (!n) {
        return NULL;
    }
    _waiter_t* w = list_entry(n, _waiter_t, node);
    list_remove(waiters, &w->node);
    return w;
}

static void _mutex_wake(scheduler_t* sched, _waiter_t* w) {
    if (w->kind == WAITER_CORO) {
        _coro_waiter_t* cw = list_entry(w, _coro_waiter_t, base);
        scheduler_schedule(sched, cw->co);
    } else {
        _thrd_waiter_t* tw = list_entry(w, _thrd_waiter_t, base);
        thrd_wake_signal(tw->wake);
    }
}

static bool _mutex_park_cb(mco_coro* co, void* arg) {
    _coro_waiter_t* w   = (_coro_waiter_t*)arg;
    xylem_mutex_t*  mtx = w->base.mtx;

    w->co = co;

    spin_lock(&mtx->guard);
    if (!atomic_load(&mtx->locked)) {
        spin_unlock(&mtx->guard);
        return false;
    }
    _mutex_push_waiter(&mtx->waiters, &w->base);
    spin_unlock(&mtx->guard);
    return true;
}

static void _mutex_wait_coro(xylem_mutex_t* mtx) {
    _coro_waiter_t w;
    w.base.kind = WAITER_CORO;
    w.base.mtx  = mtx;
    w.co        = NULL;

    for (;;) {
        scheduler_park(mtx->sched, _mutex_park_cb, &w);
        if (_mutex_try_take(mtx)) {
            return;
        }
    }
}

static void _mutex_wait_thrd(xylem_mutex_t* mtx) {
    thrd_wake_t*  wake = thrd_wake_self();
    _thrd_waiter_t w;
    w.base.kind = WAITER_THRD;
    w.base.mtx  = mtx;
    w.wake      = wake;

    for (;;) {
        if (_mutex_spin_take(mtx)) {
            return;
        }

        spin_lock(&mtx->guard);
        if (_mutex_try_take(mtx)) {
            spin_unlock(&mtx->guard);
            return;
        }
        _mutex_push_waiter(&mtx->waiters, &w.base);
        spin_unlock(&mtx->guard);

        thrd_wake_wait(wake);
        if (_mutex_try_take(mtx)) {
            return;
        }
    }
}

xylem_mutex_t* xylem_mutex_create(void) {
    xylem_mutex_t* mtx = (xylem_mutex_t*)calloc(1, sizeof(xylem_mutex_t));
    if (!mtx) {
        return NULL;
    }
    atomic_init(&mtx->locked, false);
    spin_init(&mtx->guard);
    list_init(&mtx->waiters);
    mtx->sched = runtime_get_scheduler();
    return mtx;
}

void xylem_mutex_destroy(xylem_mutex_t* mtx) {
    if (!mtx) {
        return;
    }
    free(mtx);
}

void xylem_mutex_lock(xylem_mutex_t* mtx) {
    if (_mutex_try_take(mtx)) {
        return;
    }
    if (mco_running()) {
        _mutex_wait_coro(mtx);
    } else {
        _mutex_wait_thrd(mtx);
    }
}

bool xylem_mutex_trylock(xylem_mutex_t* mtx) {
    return _mutex_try_take(mtx);
}

void xylem_mutex_unlock(xylem_mutex_t* mtx) {
    atomic_store(&mtx->locked, false);

    spin_lock(&mtx->guard);
    _waiter_t* target = _mutex_pop_waiter(&mtx->waiters);
    spin_unlock(&mtx->guard);

    if (target) {
        _mutex_wake(mtx->sched, target);
    }
    if (runtime_consume_credit(RUNTIME_CREDIT_COST)) {
        runtime_yield();
    }
}
