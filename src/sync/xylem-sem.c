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

#include "xylem/sync/xylem-sem.h"

#include "container/list.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"
#include "sync/thrd-wake.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Cross-context counting semaphore.
 *
 * `count` stores banked tokens only when no waiter is queued. Once a waiter
 * is queued, each post directly transfers ownership of one token to the
 * FIFO-oldest waiter and wakes exactly that waiter. This keeps the coroutine
 * wait path single-shot: a resumed coroutine already owns its token and does
 * not need a granted/re-contend loop.
 *
 * The `guard` serializes queue mutation with count banking. A waiter racing a
 * post either observes the banked count in its park/enqueue callback or is
 * linked before the post chooses a wake target.
 */
struct xylem_sem_s {
    spin_t           guard;
    _Atomic uint32_t count;
    _Atomic int32_t  refcnt;
    list_t           waiters;
    scheduler_t*     sched;
};

typedef enum _waiter_kind_e {
    WAITER_CORO,
    WAITER_THRD,
} _waiter_kind_t;

typedef struct _waiter_s {
    list_node_t    node;
    _waiter_kind_t kind;
    xylem_sem_t*   sem;
    bool           queued;
} _waiter_t;

typedef struct _coro_waiter_s {
    _waiter_t          base;
    mco_coro*          co;
    uint64_t           timeout_ms;
    scheduler_timer_t* timer;
    _Atomic int32_t    refcnt;
    _Atomic bool       timer_fired;
} _coro_waiter_t;

typedef struct _thrd_waiter_s {
    _waiter_t    base;
    thrd_wake_t* wake;
} _thrd_waiter_t;

static bool _sem_try_take(xylem_sem_t* s) {
    uint32_t c = atomic_load(&s->count);
    while (c > 0) {
        if (atomic_compare_exchange_weak(&s->count, &c, c - 1)) {
            return true;
        }
    }
    return false;
}

static void _sem_ref(xylem_sem_t* s) {
    atomic_fetch_add(&s->refcnt, 1);
}

static void _sem_unref(xylem_sem_t* s) {
    if (atomic_fetch_sub(&s->refcnt, 1) != 1) {
        return;
    }
    free(s);
}

static void _sem_wake(xylem_sem_t* s, _waiter_t* w) {
    if (w->kind == WAITER_CORO) {
        _coro_waiter_t* cw = list_entry(w, _coro_waiter_t, base);
        scheduler_coro_ready(s->sched, cw->co);
    } else {
        _thrd_waiter_t* tw = list_entry(w, _thrd_waiter_t, base);
        thrd_wake_signal(tw->wake);
    }
}

static void _sem_push_waiter(list_t* waiters, _waiter_t* w) {
    w->queued = true;
    list_insert_tail(waiters, &w->node);
}

static _waiter_t* _sem_pop_waiter(list_t* waiters) {
    list_node_t* n = list_head(waiters);
    if (!n) {
        return NULL;
    }
    _waiter_t* w = list_entry(n, _waiter_t, node);
    list_remove(waiters, &w->node);
    w->queued = false;
    return w;
}

static bool _sem_cancel_waiter(list_t* waiters, _waiter_t* w) {
    bool queued = w->queued;
    if (queued) {
        list_remove(waiters, &w->node);
        w->queued = false;
    }
    return queued;
}

static void _sem_coro_timed_ref(_coro_waiter_t* w) {
    atomic_fetch_add(&w->refcnt, 1);
}

static void _sem_coro_timed_unref(_coro_waiter_t* w) {
    if (atomic_fetch_sub(&w->refcnt, 1) != 1) {
        return;
    }
    if (w->timer) {
        scheduler_timer_destroy(w->timer);
    }
    free(w);
}

static void _sem_timeout_cb(scheduler_timer_t* timer, void* ud) {
    (void)timer;
    _coro_waiter_t* w      = (_coro_waiter_t*)ud;
    xylem_sem_t*    s      = w->base.sem;
    _waiter_t*      target = NULL;

    spin_lock(&s->guard);
    if (_sem_cancel_waiter(&s->waiters, &w->base)) {
        target = &w->base;
        atomic_store(&w->timer_fired, true);
    }
    spin_unlock(&s->guard);

    if (target) {
        _sem_wake(s, target);
    }
    _sem_coro_timed_unref(w);
    _sem_unref(s);
}

static bool _sem_wait_commit_cb(mco_coro* co, void* arg) {
    _coro_waiter_t* w = (_coro_waiter_t*)arg;
    xylem_sem_t*    s = w->base.sem;

    w->co = co;

    spin_lock(&s->guard);
    if (_sem_try_take(s)) {
        spin_unlock(&s->guard);
        return false;
    }
    _sem_push_waiter(&s->waiters, &w->base);
    if (w->timeout_ms > 0) {
        _sem_ref(s);
        _sem_coro_timed_ref(w);
        scheduler_timer_start(
                w->timer, _sem_timeout_cb, w, w->timeout_ms, 0);
    }
    spin_unlock(&s->guard);
    return true;
}

static void _sem_wait_thrd(xylem_sem_t* s) {
    if (_sem_try_take(s)) {
        return;
    }

    thrd_wake_t* wake = thrd_wake_self();

    _thrd_waiter_t w;
    w.base.kind = WAITER_THRD;
    w.base.sem  = s;
    w.wake      = wake;

    spin_lock(&s->guard);
    if (_sem_try_take(s)) {
        spin_unlock(&s->guard);
        return;
    }
    _sem_push_waiter(&s->waiters, &w.base);
    spin_unlock(&s->guard);

    thrd_wake_wait(wake);
}

static bool _sem_timedwait_thrd(xylem_sem_t* s, uint64_t timeout_ms) {
    if (_sem_try_take(s)) {
        return true;
    }

    thrd_wake_t* wake = thrd_wake_self();

    _thrd_waiter_t w;
    w.base.kind = WAITER_THRD;
    w.base.sem  = s;
    w.wake      = wake;

    spin_lock(&s->guard);
    if (_sem_try_take(s)) {
        spin_unlock(&s->guard);
        return true;
    }
    _sem_push_waiter(&s->waiters, &w.base);
    spin_unlock(&s->guard);

    if (thrd_wake_timedwait(wake, timeout_ms)) {
        return true;
    }

    spin_lock(&s->guard);
    bool cancelled = _sem_cancel_waiter(&s->waiters, &w.base);
    spin_unlock(&s->guard);
    if (cancelled) {
        return false;
    }

    thrd_wake_wait(wake);
    return true;
}

static void _sem_wait_coro(xylem_sem_t* s) {
    if (_sem_try_take(s)) {
        return;
    }

    _coro_waiter_t w;
    w.base.kind  = WAITER_CORO;
    w.base.sem   = s;
    w.co         = NULL;
    w.timeout_ms = 0;

    _sem_ref(s);
    scheduler_coro_park(s->sched, _sem_wait_commit_cb, &w);
    _sem_unref(s);
}

static bool _sem_timedwait_coro(xylem_sem_t* s, uint64_t timeout_ms) {
    if (_sem_try_take(s)) {
        return true;
    }

    _coro_waiter_t* w = (_coro_waiter_t*)calloc(1, sizeof(_coro_waiter_t));
    if (!w) {
        return false;
    }
    w->base.kind  = WAITER_CORO;
    w->base.sem   = s;
    w->co         = NULL;
    w->timeout_ms = timeout_ms;
    w->timer      = scheduler_timer_create(s->sched);
    if (!w->timer) {
        free(w);
        return false;
    }
    atomic_init(&w->refcnt, 1);
    atomic_init(&w->timer_fired, false);

    _sem_ref(s);
    scheduler_coro_park(s->sched, _sem_wait_commit_cb, w);

    bool fired = atomic_load(&w->timer_fired);
    if (scheduler_timer_stop(w->timer)) {
        _sem_coro_timed_unref(w);
        _sem_unref(s);
    }
    _sem_coro_timed_unref(w);
    _sem_unref(s);
    return !fired;
}

xylem_sem_t* xylem_sem_create(uint32_t value) {
    xylem_sem_t* s = (xylem_sem_t*)calloc(1, sizeof(xylem_sem_t));
    if (!s) {
        return NULL;
    }
    spin_init(&s->guard);
    atomic_init(&s->count, value);
    atomic_init(&s->refcnt, 1);
    list_init(&s->waiters);
    s->sched = runtime_get_scheduler();
    return s;
}

void xylem_sem_destroy(xylem_sem_t* s) {
    if (!s) {
        return;
    }
    _sem_unref(s);
}

void xylem_sem_wait(xylem_sem_t* s) {
    if (mco_running()) {
        _sem_wait_coro(s);
    } else {
        _sem_wait_thrd(s);
    }
}

bool xylem_sem_timedwait(xylem_sem_t* s, uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return _sem_try_take(s);
    }

    if (mco_running()) {
        return _sem_timedwait_coro(s, timeout_ms);
    } else {
        return _sem_timedwait_thrd(s, timeout_ms);
    }
}

void xylem_sem_post(xylem_sem_t* s) {
    spin_lock(&s->guard);
    _waiter_t* target = _sem_pop_waiter(&s->waiters);
    if (!target) {
        atomic_fetch_add(&s->count, 1);
    }
    spin_unlock(&s->guard);

    if (target) {
        _sem_wake(s, target);
    }
    if (runtime_consume_credit(RUNTIME_CREDIT_COST)) {
        runtime_yield();
    }
}
