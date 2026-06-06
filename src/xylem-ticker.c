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

#include "xylem/sync/xylem-channel.h"
#include "xylem/xylem-utils.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdlib.h>

struct xylem_ticker_s {
    sched_timer_t*   timer;       /* inline repeat timer (spawn == false) */
    xylem_channel_t* ch;          /* MPSC: cb sends, one consumer recvs   */
    _Atomic bool     closed;      /* set once by xylem_ticker_destroy     */
    _Atomic int32_t  pending;     /* 0/1 -- caps the channel at one tick  */
    _Atomic uint64_t last_tick;   /* most recent tick time (ms)           */
    _Atomic int32_t  refcnt;      /* lifetime, mirrors rudp/tcp sessions  */
};

/**
 * A fixed non-NULL token. The channel only carries "a tick happened";
 * the actual tick time is read from t->last_tick after recv. Using a
 * static address avoids a heap allocation per tick.
 */
static char _ticker_tick;

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
        sched_timer_destroy(t->timer);
    }
    if (t->ch) {
        /**
         * Channel was never closed while alive, so any sentinel still
         * queued is just dropped here (the token is static, not heap).
         */
        xylem_channel_destroy(t->ch);
    }
    free(t);
}

/**
 * Runs inline on the timer's owner worker (spawn == false), so it is
 * serialized against itself: ticks can never overlap or re-enter. It
 * does no blocking work and never yields -- just a coalescing,
 * non-blocking hand-off to the channel.
 */
static void _ticker_tick_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    xylem_ticker_t* t = (xylem_ticker_t*)ud;

    /**
     * Hold a reference across the whole callback. sched_timer_stop() does
     * not drain an in-flight fire, so a concurrent xylem_ticker_destroy()
     * on another worker could otherwise drop the last reference and free
     * t (and its channel) while we are still touching them below. The
     * scheduler keeps the underlying timer object alive for the duration
     * of the fire, so the sched_timer_destroy() reached through a last
     * _ticker_unref() here stays safe.
     */
    _ticker_ref(t);

    if (atomic_load_explicit(&t->closed, memory_order_acquire)) {
        _ticker_unref(t);
        return;
    }

    atomic_store_explicit(
        &t->last_tick,
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC),
        memory_order_relaxed);

    /**
     * Deliver only if the previous tick has been drained. Otherwise the
     * consumer is behind and we coalesce (drop) this tick, exactly like
     * Go's buffered-by-one Ticker channel.
     */
    int32_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &t->pending, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        xylem_channel_send(t->ch, &_ticker_tick);
    }

    _ticker_unref(t);
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

    t->ch    = xylem_channel_create();
    t->timer = sched_timer_create(sched);
    if (!t->ch || !t->timer) {
        if (t->ch) {
            xylem_channel_destroy(t->ch);
        }
        if (t->timer) {
            sched_timer_destroy(t->timer);
        }
        free(t);
        return NULL;
    }

    atomic_store_explicit(&t->refcnt, 1, memory_order_relaxed);

    /* Native repeat on the inline path; repeat+spawn could overlap cbs. */
    sched_timer_start(t->timer, _ticker_tick_cb, t, interval_ms, interval_ms);

    return t;
}

uint64_t xylem_ticker_recv(xylem_ticker_t* ticker) {
    if (!ticker) {
        return 0;
    }

    /**
     * Hold a reference across the (parking) recv so a concurrent
     * xylem_ticker_destroy cannot free the ticker out from under us.
     */
    _ticker_ref(ticker);

    uint64_t tick = 0;
    void*    msg  = xylem_channel_recv(ticker->ch);
    if (msg != NULL &&
        !atomic_load_explicit(&ticker->closed, memory_order_acquire)) {
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

    sched_timer_stop(ticker->timer);

    /**
     * Wake a consumer blocked in recv. We never close the channel (that
     * would make a racing cb send abort), so this send is always safe;
     * the consumer sees closed and returns 0.
     */
    xylem_channel_send(ticker->ch, &_ticker_tick);

    /* Drop the creator's reference. */
    _ticker_unref(ticker);
}
