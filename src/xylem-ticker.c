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

#include "xylem/xylem-ticker.h"

#include "xylem/sync/xylem-sem.h"
#include "xylem/xylem-utils.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdlib.h>

/**
 * Tick delivery rides on an xylem_sem rather than a channel: the
 * semaphore's wait() is context-adaptive (parks a coroutine, blocks an
 * OS thread), so xylem_ticker_recv works from either context -- the
 * ticker matches the underlying timer, which is usable from coroutine
 * and thread alike. The sem also avoids a per-tick heap allocation (no
 * message node) and fits the ticker's single-producer/single-consumer,
 * coalesce-to-one model: count stays in {0,1}, gated by `pending`.
 */
struct xylem_ticker_s {
    scheduler_timer_t*   timer;     /* repeating, run inline (spawn == false) */
    xylem_sem_t*     sem;
    _Atomic bool     closed;
    _Atomic int32_t  pending;   /* 0/1 coalescing cap: drop ticks when behind */
    _Atomic uint64_t last_tick;
    _Atomic int32_t  refcnt;
};

static void _ticker_ref(xylem_ticker_t* t) {
    atomic_fetch_add_explicit(&t->refcnt, 1, memory_order_relaxed);
}

static void _ticker_unref(xylem_ticker_t* t) {
    if (atomic_fetch_sub_explicit(&t->refcnt, 1, memory_order_acq_rel) != 1) {
        return;
    }
    /**
     * Last reference: cb and consumer have both released; nobody can
     * touch us any more, so it is safe to tear everything down.
     */
    if (t->timer) {
        scheduler_timer_destroy(t->timer);
    }
    if (t->sem) {
        xylem_sem_destroy(t->sem);
    }
    free(t);
}

/* ud-guard adapters: the scheduler pins the ticker across a fire via
 * these, see _ticker_tick_cb and xylem_ticker_create. */
static void _ticker_ud_ref(void* ud) {
    _ticker_ref((xylem_ticker_t*)ud);
}

static void _ticker_ud_unref(void* ud) {
    _ticker_unref((xylem_ticker_t*)ud);
}

/**
 * Runs inline on the timer's owner worker (spawn == false), so it is
 * serialized against itself: ticks can never overlap or re-enter. It
 * does no blocking work and never yields -- just a coalescing,
 * non-blocking hand-off via xylem_sem_post (any-context, never parks).
 *
 * The scheduler holds a reference on the ticker for the whole duration
 * of this callback: it calls the ud_ref hook (installed in
 * xylem_ticker_create) under its timer_lock, atomically with pulling
 * this fire off the heap, and the matching ud_unref only after we
 * return. A concurrent xylem_ticker_destroy() on another thread can
 * therefore drop the creator's reference at any moment without freeing
 * the ticker out from under us -- the last unref happens here, after the
 * callback, never mid-flight. That is why no _ticker_ref is needed in
 * the body below.
 */
static void _ticker_tick_cb(scheduler_timer_t* timer, void* ud) {
    (void)timer;
    xylem_ticker_t* t = (xylem_ticker_t*)ud;

    if (atomic_load_explicit(&t->closed, memory_order_acquire)) {
        return;
    }

    atomic_store_explicit(
        &t->last_tick,
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC),
        memory_order_relaxed);

    /**
     * Deliver only if the previous tick has been drained. Otherwise the
     * consumer is behind and we coalesce (drop) this tick, exactly like
     * Go's buffered-by-one Ticker channel. post() never fails or
     * allocates, so the slot transitions 0 -> 1 -> (recv) -> 0 cleanly.
     */
    int32_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &t->pending, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        xylem_sem_post(t->sem);
    }
}

xylem_ticker_t* xylem_ticker_create(uint64_t interval_ms) {
    scheduler_t* sched = runtime_get_scheduler();
    if (!sched || interval_ms == 0) {
        return NULL;
    }

    xylem_ticker_t* t = (xylem_ticker_t*)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }

    t->sem   = xylem_sem_create(0);
    t->timer = scheduler_timer_create(sched);
    if (!t->sem || !t->timer) {
        if (t->sem) {
            xylem_sem_destroy(t->sem);
        }
        if (t->timer) {
            scheduler_timer_destroy(t->timer);
        }
        free(t);
        return NULL;
    }

    atomic_store_explicit(&t->refcnt, 1, memory_order_relaxed);

    /* Native repeat on the inline path; repeat+spawn could overlap cbs. */
    scheduler_timer_set_ud_guard(t->timer, _ticker_ud_ref, _ticker_ud_unref);
    if (scheduler_timer_start(
            t->timer, _ticker_tick_cb, t, interval_ms, interval_ms) != 0) {
        scheduler_timer_destroy(t->timer);
        xylem_sem_destroy(t->sem);
        free(t);
        return NULL;
    }

    return t;
}

uint64_t xylem_ticker_recv(xylem_ticker_t* ticker) {
    if (!ticker) {
        return 0;
    }

    /**
     * Hold a reference across the (blocking) wait so a concurrent
     * xylem_ticker_destroy cannot free the ticker out from under us.
     * xylem_sem_wait adapts to the caller: it parks a coroutine or
     * blocks an OS thread, so recv works from either context.
     */
    _ticker_ref(ticker);

    uint64_t tick = 0;
    xylem_sem_wait(ticker->sem);
    if (!atomic_load_explicit(&ticker->closed, memory_order_acquire)) {
        /* Allow the next tick through; re-opens the coalescing slot. */
        atomic_store_explicit(&ticker->pending, 0, memory_order_release);
        tick = atomic_load_explicit(&ticker->last_tick, memory_order_relaxed);
    }

    _ticker_unref(ticker);
    return tick;
}

void xylem_ticker_destroy(xylem_ticker_t* ticker) {
    if (!ticker) {
        return;
    }
    if (atomic_exchange_explicit(&ticker->closed, true, memory_order_acq_rel)) {
        return; /* already stopped */
    }

    scheduler_timer_stop(ticker->timer);

    /**
     * Wake a consumer blocked in recv; it sees `closed` and returns 0.
     * post() is any-context and idempotent enough here: an extra token
     * just makes the next (already-closed) wait return immediately. We
     * set `closed` before posting so the woken consumer cannot mistake
     * this wake for a real tick.
     */
    xylem_sem_post(ticker->sem);

    /* Drop the creator's reference. */
    _ticker_unref(ticker);
}
