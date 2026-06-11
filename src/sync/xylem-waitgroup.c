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
#include "platform/platform-futex.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Cross-context countdown latch (Go's sync.WaitGroup).
 *
 * `cnt` is the lock-free latch counter; the predicate every waiter blocks
 * on is "cnt == 0". The two waiter kinds block differently, mirroring
 * xylem_sem / xylem_mutex:
 *
 *   - Coroutines queue FIFO on `co_waiters` (guarded by `guard`) and park
 *     on the scheduler. done()-at-zero drains the list and reschedules
 *     each, with no kernel round-trip.
 *
 *   - OS threads do not queue: they barge on `gate`, a generation word
 *     that done()-at-zero bumps and broadcasts. A thread sleeps in
 *     platform_futex_wait while cnt != 0; the futex value-compare against
 *     the snapshotted generation closes the lost-wakeup race (a done()
 *     that opens the latch between the snapshot and the sleep changes
 *     `gate`, so the wait returns at once). `thr_waiters` tells done()
 *     whether a broadcast is needed, so a coroutine-only workload pays no
 *     syscall.
 *
 * `cnt` is size_t (the public counter width); a futex word must be 32-bit,
 * so threads block on the separate `gate` generation rather than on `cnt`
 * directly. `guard` keeps the coroutine list coherent with the latch: a
 * waiter re-checks cnt under it before enqueuing, and done() drains under
 * it, so a done() racing an enqueue can never strand a coroutine. Threads
 * need no guard -- futex_wait re-checks, and the seq_cst pair between
 * done()'s gate bump and a thread's arm closes its race.
 *
 * Reuse (add() after the latch reached zero) follows Go's contract: such
 * add()s must happen-after all prior wait()s returned.
 */
struct xylem_waitgroup_s {
    atomic_size_t    cnt;
    _Atomic uint32_t gate;        /* generation; bumped when latch opens */
    _Atomic int32_t  thr_waiters; /* OS threads blocked on the futex      */
    spin_t           guard;       /* serialises the coroutine list        */
    list_t           co_waiters;  /* coroutine waiters, FIFO              */
};

/**
 * Coroutine waiter record, embedded in the waiting coroutine's wait()
 * frame (which stays alive while parked, so done() may read it).
 */
typedef struct _wg_co_waiter_s {
    list_node_t  node;
    mco_coro*    co;
    scheduler_t* sched;
} _wg_co_waiter_t;

typedef struct _wg_park_ctx_s {
    xylem_waitgroup_t* wg;
    _wg_co_waiter_t*   w;
} _wg_park_ctx_t;

/**
 * Enqueue @p node only if the latch is still closed. Declining (false)
 * when cnt already reached zero is what closes the race with done(): the
 * caller then runs inline instead of waiting for a wake that may never
 * come.
 */
static bool _wg_enqueue(xylem_waitgroup_t* wg, list_node_t* node) {
    spin_lock(&wg->guard);
    if (atomic_load_explicit(&wg->cnt, memory_order_acquire) == 0) {
        spin_unlock(&wg->guard);
        return false;
    }
    list_insert_tail(&wg->co_waiters, node);
    spin_unlock(&wg->guard);
    return true;
}

static bool _wg_park_cb(mco_coro* co, void* arg) {
    _wg_park_ctx_t* ctx = (_wg_park_ctx_t*)arg;

    /* co is known only now that the coroutine has actually suspended. */
    ctx->w->co = co;
    return _wg_enqueue(ctx->wg, &ctx->w->node);
}

/**
 * Reschedule every coroutine in a drained list, in FIFO order. Reads each
 * node's successor before waking it, since the woken coroutine's frame may
 * vanish on resume.
 */
static void _wg_wake_all(list_t* drained) {
    list_node_t* sentinel = list_sentinel(drained);
    list_node_t* n        = list_head(drained);
    while (n) {
        list_node_t* next = list_next(n);
        if (next == sentinel) {
            next = NULL;
        }
        _wg_co_waiter_t* w = list_entry(n, _wg_co_waiter_t, node);
        scheduler_schedule(w->sched, w->co);
        n = next;
    }
}

xylem_waitgroup_t* xylem_waitgroup_create(void) {
    xylem_waitgroup_t* wg =
        (xylem_waitgroup_t*)calloc(1, sizeof(xylem_waitgroup_t));
    if (!wg) {
        return NULL;
    }
    atomic_init(&wg->cnt, 0);
    atomic_init(&wg->gate, 0);
    atomic_init(&wg->thr_waiters, 0);
    spin_init(&wg->guard);
    list_init(&wg->co_waiters);
    return wg;
}

void xylem_waitgroup_destroy(xylem_waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    free(wg);
}

void xylem_waitgroup_add(xylem_waitgroup_t* wg, size_t delta) {
    /* Lock-free: neither parks nor wakes, so safe from any context. */
    atomic_fetch_add(&wg->cnt, delta);
}

void xylem_waitgroup_done(xylem_waitgroup_t* wg) {
    size_t prev = atomic_fetch_sub(&wg->cnt, 1);
    if (prev == 0) {
        /* Underflow: more done() than add(); mirrors Go's panic. */
        xylem_loge("<waitgroup> done called with zero counter wg=%p",
                   (void*)wg);
        abort();
    }
    if (prev != 1) {
        return;
    }

    /* Latch reached zero. Wake coroutines: drain under the guard, wake
     * outside it. */
    list_t drained;
    list_init(&drained);

    spin_lock(&wg->guard);
    list_swap(&drained, &wg->co_waiters);
    spin_unlock(&wg->guard);

    _wg_wake_all(&drained);

    /* Wake threads: open a new generation, then broadcast if any sleep.
     * seq_cst pairs with a thread's arm so the wake is never skipped while
     * a thread sleeps on a closed gate. */
    atomic_fetch_add_explicit(&wg->gate, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&wg->thr_waiters, memory_order_seq_cst) > 0) {
        platform_futex_broadcast(&wg->gate);
    }
}

void xylem_waitgroup_wait(xylem_waitgroup_t* wg) {
    if (atomic_load_explicit(&wg->cnt, memory_order_acquire) == 0) {
        return;
    }

    if (mco_running()) {
        /* Coroutine: park on a stack waiter (park cb re-checks the latch). */
        _wg_co_waiter_t w;
        w.co    = NULL;
        w.sched = runtime_get_scheduler();

        _wg_park_ctx_t ctx = { wg, &w };
        scheduler_park(w.sched, _wg_park_cb, &ctx);
        return;
    }

    /* OS thread: barge on the gate futex word until the latch opens.
     * Snapshot the generation, re-test cnt, then sleep only while the
     * gate is unchanged -- a done() that opened the latch in between bumps
     * the gate, so the wait returns immediately. */
    atomic_fetch_add_explicit(&wg->thr_waiters, 1, memory_order_seq_cst);
    for (;;) {
        uint32_t g = atomic_load_explicit(&wg->gate, memory_order_seq_cst);
        if (atomic_load_explicit(&wg->cnt, memory_order_seq_cst) == 0) {
            break;
        }
        platform_futex_wait(&wg->gate, g);
    }
    atomic_fetch_sub_explicit(&wg->thr_waiters, 1, memory_order_relaxed);
}
