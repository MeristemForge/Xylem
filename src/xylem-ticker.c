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
 * coalesce-to-one model.
 *
 * In normal operation the sem count is bounded to {0,1}, gated by
 * `pending`. A racing xylem_ticker_close can cause a transient
 * double-post (the callback and close each call xylem_sem_post),
 * which is harmless: the consumer sees `closed` and discards the extra
 * token. close wakes a blocked consumer; destroy consumes the caller's
 * handle after the caller has stopped admission of new recv operations.
 */
struct xylem_ticker_s {
    scheduler_timer_t*   timer;     /* repeating, run inline (spawn == false) */
    xylem_sem_t*         sem;
    _Atomic bool         closed;
    _Atomic int32_t      pending;   /* 0/1 coalescing cap: drop ticks when behind */
    _Atomic uint64_t     last_tick;
    _Atomic int32_t      refcnt;
};

static void _ticker_ref(void* ud) {
    xylem_ticker_t* t = (xylem_ticker_t*)ud;
    atomic_fetch_add(&t->refcnt, 1);
}

static void _ticker_unref(void* ud) {
    xylem_ticker_t* t = (xylem_ticker_t*)ud;
    if (atomic_fetch_sub(&t->refcnt, 1) != 1) {
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

/**
 * Runs inline on whichever worker fires the timer (spawn == false), so
 * the scheduler serializes it against itself: ticks can never overlap
 * or re-enter. It does no blocking work and never yields -- just a
 * coalescing, non-blocking hand-off via xylem_sem_post (any-context,
 * never parks).
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

    if (atomic_load(&t->closed)) {
        return;
    }

    /**
     * Deliver only if the previous tick has been drained. Otherwise the
     * consumer is behind and we coalesce (drop) this tick, exactly like
     * Go's buffered-by-one Ticker channel.
     *
     * A concurrent xylem_ticker_destroy may have already posted the
     * wakeup sem after setting closed but before this callback runs.
     * That TOCTOU is harmless -- the callback's own post is extra, but
     * the consumer sees closed and discards it. The refcount (held by
     * the scheduler across the callback) guarantees the ticker is still
     * alive here even if the creator already called destroy.
     */
    int32_t expected = 0;
    if (atomic_compare_exchange_strong(&t->pending, &expected, 1)) {
        atomic_store(&t->last_tick,
                     xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
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

    atomic_store(&t->refcnt, 1);

    /* Native repeat on the inline path; the callback is small and ordered. */
    scheduler_timer_set_ud_guard(t->timer, _ticker_ref, _ticker_unref);
    /* start never fails for a valid timer; timer just created above. */
    scheduler_timer_start(t->timer, _ticker_tick_cb, t, interval_ms, interval_ms);

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
    if (!atomic_load(&ticker->closed)) {
        xylem_sem_wait(ticker->sem);
    }
    if (!atomic_load(&ticker->closed)) {
        tick = atomic_load(&ticker->last_tick);
    }
    /* Re-open the coalescing slot after reading or discarding its payload. */
    atomic_store(&ticker->pending, 0);

    _ticker_unref(ticker);
    return tick;
}

void xylem_ticker_close(xylem_ticker_t* ticker) {
    if (!ticker) {
        return;
    }
    if (atomic_exchange(&ticker->closed, true)) {
        return; /* already stopped */
    }

    scheduler_timer_stop(ticker->timer);

    /**
     * Wake a consumer blocked in recv; it sees `closed` and returns 0.
     *
     * The tick callback _ticker_tick_cb may race through its closed
     * check before this exchange and post the sem as well, resulting in
     * two wake tokens. That is harmless -- the extra token makes the
     * next (already-closed) wait return immediately, and the consumer
     * discards both. Setting `closed` before the post guarantees the
     * consumer cannot mistake any wake for a real tick.
     */
    xylem_sem_post(ticker->sem);
}

void xylem_ticker_destroy(xylem_ticker_t* ticker) {
    if (!ticker) {
        return;
    }
    xylem_ticker_close(ticker);

    /* Drop the creator's reference. */
    _ticker_unref(ticker);
}
