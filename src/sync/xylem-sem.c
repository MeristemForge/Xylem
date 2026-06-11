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
#include "xylem/xylem-utils.h"

#include "container/list.h"
#include "platform/platform-futex.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Cross-context counting semaphore.
 *
 * The one primitive both coroutines and plain OS threads may block on,
 * so either can notify the other; the rest are coroutine-only.
 *
 * Two waiter kinds block differently, mirroring xylem_mutex:
 *
 *   - Coroutines queue FIFO on `co_waiters` (guarded by `guard`) and park on
 *     the scheduler. post() hands a token to the oldest by waking it
 *     directly, never touching `count`, so the woken coroutine returns
 *     with its token in hand and none is lost -- no kernel round-trip.
 *
 *   - OS threads do not queue: they barge on `count` (the futex word),
 *     CAS-decrementing a free token or sleeping in platform_futex_wait
 *     while it is zero. post() banks the token (count++) and wakes one via
 *     platform_futex_signal. `thr_waiters` only tells post whether a futex
 *     wake is needed, so a coroutine-only workload pays no syscall.
 *
 * `guard` serialises the post decision (hand to a coroutine vs. bank the
 * token) against a coroutine enqueuing in its park callback: the bank
 * (count++) happens under the guard, so a coroutine racing post is either
 * seen in the list (handed the token) or observes the banked count in its
 * park callback (takes it) -- never stranded. Threads need no guard: a
 * barge that beats the bank just re-checks the word, and the seq_cst pair
 * between post's count++ and a thread's arm closes the lost-wakeup race.
 *
 * Waiter lifetime: infinite coroutine waits keep the record on the parked
 * coroutine's stack (post snapshots it under the guard, never derefs it
 * after). A timed coroutine wait can be pulled from the queue by a timer
 * on another worker racing the resumed coroutine, so it is a refcounted
 * heap object (wait ref + timer ref) freed by whoever drops the last ref.
 */

struct xylem_sem_s {
    spin_t           guard;       /* serialises coro list + the count bank */
    _Atomic uint32_t count;       /* tokens; also the thread futex word    */
    _Atomic int32_t  thr_waiters; /* OS threads sleeping on the futex word  */
    list_t           co_waiters;     /* coroutine waiters, FIFO                */
};

static uint64_t _sem_now_ms(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
}

/**
 * Take a token if one is free: lock-free CAS-decrement of `count`, valid
 * in any context (coroutine fast path, thread barge, non-blocking try).
 */
static bool _sem_try(xylem_sem_t* s) {
    uint32_t c = atomic_load_explicit(&s->count, memory_order_acquire);
    while (c > 0) {
        if (atomic_compare_exchange_weak_explicit(
                &s->count, &c, c - 1,
                memory_order_acquire, memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

/**
 * A NULL next link means the node was never linked or was removed
 * (list_remove clears it). Read under the guard to arbitrate the
 * timeout-vs-post race.
 */
static inline bool _sem_node_linked(const list_node_t* n) {
    return n->next != NULL;
}

/**
 * Coroutine waiter record. Lives in the waiting coroutine's lock frame
 * (infinite wait) or inside the refcounted timed object below. Threads
 * never queue here -- they barge on `count`.
 */
typedef struct _sem_co_waiter_s {
    list_node_t  node;
    mco_coro*    co;
    scheduler_t* sched;
} _sem_co_waiter_t;

/* Timed coroutine waiter: refcounted heap object (sem is its only user). */

typedef struct _sem_co_timed_s {
    _sem_co_waiter_t base;
    xylem_sem_t*     sem;
    sched_timer_t*   timer;  /* NULL if creation failed */
    uint64_t         timeout_ms;
    _Atomic int32_t  refcnt; /* wait ref + timer ref while armed */
    _Atomic bool     timed_out;
} _sem_co_timed_t;

static void _sem_co_unref(_sem_co_timed_t* w) {
    if (atomic_fetch_sub_explicit(&w->refcnt, 1, memory_order_acq_rel) != 1) {
        return;
    }
    /* The last ref owns the timer; destroy is safe even mid-callback. */
    if (w->timer) {
        sched_timer_destroy(w->timer);
    }
    free(w);
}

static void _sem_co_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    _sem_co_timed_t* w = (_sem_co_timed_t*)ud;
    xylem_sem_t*     s = w->sem;

    /**
     * If the node is already gone, a post() won the hand-off and will
     * resume the coroutine; scheduling it again would be a double wake.
     */
    spin_lock(&s->guard);
    bool linked = _sem_node_linked(&w->base.node);
    if (linked) {
        list_remove(&s->co_waiters, &w->base.node);
        atomic_store_explicit(&w->timed_out, true, memory_order_relaxed);
    }
    _sem_co_waiter_t target = w->base;
    spin_unlock(&s->guard);

    /* Last touch of the coroutine: it may drop its ref and free w. */
    if (linked) {
        scheduler_schedule(target.sched, target.co);
    }
    _sem_co_unref(w);
}

typedef struct _sem_timed_ctx_s {
    xylem_sem_t*     sem;
    _sem_co_timed_t* w;
} _sem_timed_ctx_t;

static bool _sem_timed_park_cb(mco_coro* co, void* arg) {
    _sem_timed_ctx_t* ctx = (_sem_timed_ctx_t*)arg;
    xylem_sem_t*      s   = ctx->sem;
    _sem_co_timed_t*  w   = ctx->w;

    w->base.co = co;

    spin_lock(&s->guard);
    /* A post() may have banked a token since the fast path; take it. */
    if (_sem_try(s)) {
        spin_unlock(&s->guard);
        return false;
    }

    list_insert_tail(&s->co_waiters, &w->base.node);

    /**
     * Arm under the guard so a post() blocked on it cannot resume this
     * coroutine before the timer is live. sched_timer_start only
     * touches the timer heap, so holding the spin across it is safe.
     */
    atomic_fetch_add_explicit(&w->refcnt, 1, memory_order_relaxed);
    sched_timer_start(w->timer, _sem_co_timeout_cb, w, w->timeout_ms, 0);
    spin_unlock(&s->guard);
    return true;
}

/* Infinite coroutine waiter: stack record, parked via the scheduler. */

typedef struct _sem_inf_ctx_s {
    xylem_sem_t*      sem;
    _sem_co_waiter_t* w;
} _sem_inf_ctx_t;

static bool _sem_inf_park_cb(mco_coro* co, void* arg) {
    _sem_inf_ctx_t* ctx = (_sem_inf_ctx_t*)arg;
    xylem_sem_t*    s   = ctx->sem;

    ctx->w->co = co;

    spin_lock(&s->guard);
    /* A post() may have banked a token since the fast path; take it. */
    if (_sem_try(s)) {
        spin_unlock(&s->guard);
        return false;
    }
    list_insert_tail(&s->co_waiters, &ctx->w->node);
    spin_unlock(&s->guard);
    return true;
}

xylem_sem_t* xylem_sem_create(unsigned int value) {
    xylem_sem_t* s = (xylem_sem_t*)calloc(1, sizeof(xylem_sem_t));
    if (!s) {
        return NULL;
    }
    spin_init(&s->guard);
    atomic_init(&s->count, value);
    atomic_init(&s->thr_waiters, 0);
    list_init(&s->co_waiters);
    return s;
}

void xylem_sem_destroy(xylem_sem_t* s) {
    if (!s) {
        return;
    }
    free(s);
}

/**
 * Thread acquire: barge on the futex word. CAS-decrement a free token or
 * sleep while it is zero; `expected == 0` makes the wait a no-op if a
 * post() banked a token between the load and the sleep. thr_waiters is
 * published with seq_cst so post() cannot skip our wake (the store pairs
 * with post()'s seq_cst count bump + thr_waiters read).
 *
 * @return true if a token was taken, false only when @p deadline_ms is
 *         non-zero and the deadline elapsed first.
 */
static bool _sem_wait_thread(xylem_sem_t* s, bool timed, uint64_t timeout_ms) {
    uint64_t deadline = timed ? _sem_now_ms() + timeout_ms : 0;

    atomic_fetch_add_explicit(&s->thr_waiters, 1, memory_order_seq_cst);
    bool got = false;
    for (;;) {
        uint32_t c = atomic_load_explicit(&s->count, memory_order_seq_cst);
        if (c > 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &s->count, &c, c - 1,
                    memory_order_acquire, memory_order_seq_cst)) {
                got = true;
                break;
            }
            continue; /* lost the race for this token; reload and retry */
        }
        if (!timed) {
            platform_futex_wait(&s->count, 0);
            continue;
        }
        uint64_t now = _sem_now_ms();
        if (now >= deadline) {
            break; /* timed out */
        }
        platform_futex_timedwait(&s->count, 0, deadline - now);
    }
    atomic_fetch_sub_explicit(&s->thr_waiters, 1, memory_order_relaxed);
    return got;
}

void xylem_sem_wait(xylem_sem_t* s) {
    if (mco_running()) {
        /* Coroutine: park on a stack waiter (park cb re-checks the token). */
        if (_sem_try(s)) {
            return;
        }

        _sem_co_waiter_t w;
        w.co    = NULL;
        w.sched = runtime_get_scheduler();

        _sem_inf_ctx_t ctx = { s, &w };
        scheduler_park(w.sched, _sem_inf_park_cb, &ctx);
        return;
    }

    /* Thread: barge on the futex word until a token is taken. */
    (void)_sem_wait_thread(s, false, 0);
}

bool xylem_sem_timedwait(xylem_sem_t* s, uint64_t timeout_ms) {
    /* Zero timeout is a non-blocking try in any context. */
    if (timeout_ms == 0) {
        return _sem_try(s);
    }

    if (mco_running()) {
        if (_sem_try(s)) {
            return true;
        }

        _sem_co_timed_t* w =
            (_sem_co_timed_t*)calloc(1, sizeof(_sem_co_timed_t));
        if (!w) {
            /* Cannot honour a timeout we cannot arm; fail closed. */
            return false;
        }
        w->base.co    = NULL;
        w->base.sched = runtime_get_scheduler();
        w->sem        = s;
        w->timeout_ms = timeout_ms;
        w->timer      = sched_timer_create(w->base.sched);
        if (!w->timer) {
            /* No timer means no deadline; fail closed over an unbounded wait. */
            free(w);
            return false;
        }
        atomic_init(&w->refcnt, 1);
        atomic_init(&w->timed_out, false);

        _sem_timed_ctx_t ctx = { s, w };
        scheduler_park(w->base.sched, _sem_timed_park_cb, &ctx);

        /**
         * Woken by a post() (not timed out) or the timer (timed out).
         * Cancel a still-pending timer; a true from stop() means we
         * caught it before it fired and own its ref.
         */
        if (sched_timer_stop(w->timer)) {
            _sem_co_unref(w);
        }
        bool ok = !atomic_load_explicit(&w->timed_out, memory_order_acquire);
        _sem_co_unref(w);
        return ok;
    }

    /* External thread with deadline: barge on the futex word. */
    return _sem_wait_thread(s, true, timeout_ms);
}

void xylem_sem_post(xylem_sem_t* s) {
    spin_lock(&s->guard);
    list_node_t* n = list_head(&s->co_waiters);
    if (n) {
        /**
         * Direct hand-off to the FIFO-oldest coroutine: the token rides
         * the wake, count stays untouched, none is lost. Read co/sched
         * out before unlocking -- the waiter frame may vanish on resume.
         */
        list_remove(&s->co_waiters, n);
        _sem_co_waiter_t* cw    = list_entry(n, _sem_co_waiter_t, node);
        scheduler_t*      sched = cw->sched;
        mco_coro*         co    = cw->co;
        spin_unlock(&s->guard);
        scheduler_schedule(sched, co);
        return;
    }

    /**
     * No coroutine waiter: bank the token under the guard so a coroutine
     * racing in its park callback either is seen above or observes the
     * banked count. seq_cst pairs with a thread waiter's arm so the futex
     * wake below is never skipped while a thread sleeps on a zero word.
     */
    atomic_fetch_add_explicit(&s->count, 1, memory_order_seq_cst);
    spin_unlock(&s->guard);

    if (atomic_load_explicit(&s->thr_waiters, memory_order_seq_cst) > 0) {
        platform_futex_signal(&s->count);
    }
}
