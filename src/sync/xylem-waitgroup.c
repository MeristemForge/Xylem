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
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"
#include "sync/thrd-wake.h"

#include "runtime/minicoro/minicoro.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* count and waiter transfer share a guard; wake draining no longer touches wg. */
struct xylem_waitgroup_s {
    size_t       count;
    spin_t       guard;
    list_t       waiters;
    scheduler_t* sched;
};

typedef enum _waiter_kind_e {
    WAITER_CORO,
    WAITER_THRD,
} _waiter_kind_t;

typedef struct _waiter_s {
    list_node_t        node;
    _waiter_kind_t     kind;
    xylem_waitgroup_t* wg;
} _waiter_t;

typedef struct _coro_waiter_s {
    _waiter_t base;
    mco_coro* co;
} _coro_waiter_t;

typedef struct _thrd_waiter_s {
    _waiter_t    base;
    thrd_wake_t* wake;
} _thrd_waiter_t;

static void _wg_push_waiter(list_t* waiters, _waiter_t* w) {
    list_insert_tail(waiters, &w->node);
}

static _waiter_t* _wg_pop_waiter(list_t* waiters) {
    list_node_t* n = list_head(waiters);
    if (!n) {
        return NULL;
    }
    _waiter_t* w = list_entry(n, _waiter_t, node);
    list_remove(waiters, &w->node);
    return w;
}

static bool _wg_wait_commit_cb(mco_coro* co, void* arg) {
    _coro_waiter_t*    w  = (_coro_waiter_t*)arg;
    xylem_waitgroup_t* wg = w->base.wg;

    w->co = co;

    spin_lock(&wg->guard);
    if (wg->count == 0) {
        spin_unlock(&wg->guard);
        return false;
    }
    _wg_push_waiter(&wg->waiters, &w->base);
    spin_unlock(&wg->guard);
    return true;
}

static void _wg_wake(scheduler_t* sched, _waiter_t* w) {
    if (w->kind == WAITER_CORO) {
        _coro_waiter_t* cw = list_entry(w, _coro_waiter_t, base);
        scheduler_coro_ready(sched, cw->co);
    } else {
        _thrd_waiter_t* tw = list_entry(w, _thrd_waiter_t, base);
        thrd_wake_signal(tw->wake);
    }
}

static void _wg_wake_all(scheduler_t* sched, list_t* wake_list) {
    for (;;) {
        _waiter_t* w = _wg_pop_waiter(wake_list);
        if (!w) {
            return;
        }
        _wg_wake(sched, w);
    }
}

static void _wg_wait_coro(xylem_waitgroup_t* wg) {
    _coro_waiter_t w;
    w.base.kind = WAITER_CORO;
    w.base.wg   = wg;
    w.co        = NULL;

    scheduler_coro_park(wg->sched, _wg_wait_commit_cb, &w);
}

static void _wg_wait_thrd(xylem_waitgroup_t* wg) {
    thrd_wake_t*  wake = thrd_wake_self();
    _thrd_waiter_t w;
    w.base.kind = WAITER_THRD;
    w.base.wg   = wg;
    w.wake      = wake;

    spin_lock(&wg->guard);
    if (wg->count != 0) {
        _wg_push_waiter(&wg->waiters, &w.base);
        spin_unlock(&wg->guard);
        thrd_wake_wait(wake);
        return;
    }
    spin_unlock(&wg->guard);
}

xylem_waitgroup_t* xylem_waitgroup_create(void) {
    xylem_waitgroup_t* wg =
        (xylem_waitgroup_t*)calloc(1, sizeof(xylem_waitgroup_t));
    if (!wg) {
        return NULL;
    }
    wg->count = 0;
    spin_init(&wg->guard);
    list_init(&wg->waiters);
    wg->sched = runtime_get_scheduler();
    return wg;
}

void xylem_waitgroup_destroy(xylem_waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    free(wg);
}

void xylem_waitgroup_add(xylem_waitgroup_t* wg, size_t delta) {
    spin_lock(&wg->guard);
    wg->count += delta;
    spin_unlock(&wg->guard);
}

void xylem_waitgroup_done(xylem_waitgroup_t* wg) {
    list_t       wake_list;
    scheduler_t* sched = NULL;
    list_init(&wake_list);

    spin_lock(&wg->guard);
    if (wg->count == 0) {
        xylem_loge("<waitgroup> done called with zero counter wg=%p",
                   (void*)wg);
        abort();
    }
    wg->count--;
    if (wg->count == 0) {
        list_swap(&wake_list, &wg->waiters);
        sched = wg->sched;
    }
    spin_unlock(&wg->guard);

    if (!list_empty(&wake_list)) {
        _wg_wake_all(sched, &wake_list);
    }
    if (runtime_consume_step()) {
        runtime_yield();
    }
}

void xylem_waitgroup_wait(xylem_waitgroup_t* wg) {
    if (mco_running()) {
        _wg_wait_coro(wg);
    } else {
        _wg_wait_thrd(wg);
    }
}
