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

#include "xylem/sync/xylem-channel.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "platform/platform-sem.h"
#include "runtime/precond.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "container/mpsc.h"
#include "sync/waiter.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct _channel_msg_s {
    mpsc_node_t node;
    void*       payload;
} _channel_msg_t;

/**
 * MPSC channel, lock-free data path.
 *
 * Messages live in an intrusive MPSC queue (multi-producer, single
 * consumer). The single receiver -- a coroutine or an OS thread --
 * publishes itself into one atomic `waiter` slot when it has to block;
 * a sender, close(), or the deadline timer arbitrate the wakeup with a
 * single atomic_exchange on that slot, so exactly one of them resumes
 * the receiver and none double-wakes. The shared `waiter` module
 * supplies the cross-context waiter representation, the per-thread wake
 * semaphore, and the wake dispatch.
 *
 * refcnt keeps the channel alive across a concurrent destroy: the
 * creator holds one ref, every in-flight send/recv holds one, and an
 * armed deadline timer holds one (so its callback can touch the channel
 * even if recv already returned). Channel must not outlive xylem_run
 * (the deadline timer is scheduler-owned).
 *
 * count is maintained for xylem_channel_len; the capacity gate is the
 * atomic fetch_add reservation in send.
 */
struct xylem_channel_s {
    mpsc_t             queue;
    _Atomic(waiter_t*) waiter;         /* single receiver, NULL if none  */
    _Atomic bool       closed;
    _Atomic bool       recv_active;    /* single-receiver enforcement     */
    _Atomic bool       timed_out;      /* current timed coroutine recv    */
    _Atomic int32_t    refcnt;
    sched_timer_t*     deadline_timer; /* lazily created, reused          */
    size_t             cap;            /* 0 = unbounded; immutable         */
    _Atomic size_t     count;          /* queued messages, for len()       */
};

static inline void _channel_ref(xylem_channel_t* ch) {
    atomic_fetch_add_explicit(&ch->refcnt, 1, memory_order_relaxed);
}

static void _channel_unref(xylem_channel_t* ch) {
    if (atomic_fetch_sub_explicit(
            &ch->refcnt, 1, memory_order_acq_rel) != 1) {
        return;
    }
    if (ch->deadline_timer) {
        sched_timer_destroy(ch->deadline_timer);
    }
    mpsc_node_t* node;
    while ((node = mpsc_pop(&ch->queue)) != NULL) {
        _channel_msg_t* msg = mpsc_entry(node, _channel_msg_t, node);
        free(msg);
    }
    free(ch);
}

/* Wake the parked receiver, if any. Used by send, close, and the timer.
 * The atomic_exchange gives the caller exclusive ownership of the
 * wakeup: the loser sees NULL and the woken party's stack waiter stays
 * valid until it is resumed exactly once. */
static void _channel_wake_receiver(xylem_channel_t* ch) {
    waiter_t* w = atomic_exchange(&ch->waiter, NULL);
    if (w) {
        waiter_wake(*w);
    }
}

/* Coroutine receiver: park on a stack waiter, lock-free. */

typedef struct _channel_park_ctx_s {
    xylem_channel_t* ch;
    waiter_t*        w;
} _channel_park_ctx_t;

static bool _channel_park_cb(mco_coro* co, void* arg) {
    _channel_park_ctx_t* ctx = (_channel_park_ctx_t*)arg;
    xylem_channel_t*     ch  = ctx->ch;

    ctx->w->co = co;
    atomic_store_explicit(&ch->waiter, ctx->w, memory_order_release);

    /**
     * A sender / close / timer may have raced between the emptiness
     * test and publishing the waiter. Decline park so the recv loop
     * retries. Peek only -- pop+re-push would break FIFO.
     */
    if (atomic_load_explicit(&ch->closed, memory_order_acquire)
        || atomic_load_explicit(&ch->timed_out, memory_order_acquire)
        || !mpsc_empty(&ch->queue)) {
        waiter_t* expected = ctx->w;
        if (atomic_compare_exchange_strong(
                &ch->waiter, &expected, NULL)) {
            return false;
        }
    }
    return true;
}

static void _channel_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    xylem_channel_t* ch = (xylem_channel_t*)ud;
    atomic_store_explicit(&ch->timed_out, true, memory_order_release);
    _channel_wake_receiver(ch);
    /* Balance the ref recv handed us when arming the timer. */
    _channel_unref(ch);
}

static void* _channel_take(xylem_channel_t* ch, mpsc_node_t* node) {
    _channel_msg_t* m       = mpsc_entry(node, _channel_msg_t, node);
    void*           payload = m->payload;
    free(m);
    atomic_fetch_sub_explicit(&ch->count, 1, memory_order_relaxed);
    return payload;
}

static void* _channel_recv_coro(xylem_channel_t* ch, uint64_t timeout_ms) {
    bool infinite = (timeout_ms == (uint64_t)-1);

    /* Lazily create; reused across calls (hot path in DTLS/RUDP). */
    if (!infinite && !ch->deadline_timer) {
        ch->deadline_timer = sched_timer_create(runtime_get_scheduler());
    }
    uint64_t deadline_ms = infinite
        ? 0
        : xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;

    atomic_store_explicit(&ch->timed_out, false, memory_order_release);

    waiter_t w;
    waiter_init(&w);

    void* payload = NULL;
    for (;;) {
        mpsc_node_t* node = mpsc_pop(&ch->queue);
        if (node) {
            payload = _channel_take(ch, node);
            break;
        }
        if (atomic_load_explicit(&ch->closed, memory_order_acquire)) {
            break;
        }

        if (deadline_ms > 0) {
            uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (now >= deadline_ms) {
                break;
            }
            if (ch->deadline_timer) {
                /* Ref for the timer callback -- balanced by the callback
                 * itself, or by us if stop() cancels it. */
                _channel_ref(ch);
                sched_timer_start(ch->deadline_timer, _channel_timeout_cb,
                                  ch, deadline_ms - now, 0);
            }
        }

        _channel_park_ctx_t ctx = { ch, &w };
        scheduler_park(runtime_get_scheduler(), _channel_park_cb, &ctx);

        if (deadline_ms > 0 && ch->deadline_timer) {
            /* stop() true => callback will not run, we own its ref. */
            if (sched_timer_stop(ch->deadline_timer)) {
                _channel_unref(ch);
            }
        }

        /* Data wins over timeout -- avoid stranding a message that
         * arrived in the same window the deadline fired. */
        node = mpsc_pop(&ch->queue);
        if (node) {
            payload = _channel_take(ch, node);
            break;
        }
        if (atomic_load_explicit(&ch->timed_out, memory_order_acquire)) {
            break;
        }
    }
    return payload;
}

/* OS-thread receiver: block on the per-thread sem, lock-free. */

static void* _channel_recv_thread(xylem_channel_t* ch, uint64_t timeout_ms) {
    bool infinite = (timeout_ms == (uint64_t)-1);
    uint64_t deadline_ms = infinite
        ? 0
        : xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;

    waiter_t w;
    waiter_init(&w);
    if (!w.tsem) {
        /* No wake sem (OOM): best-effort single pop, never block. */
        mpsc_node_t* node = mpsc_pop(&ch->queue);
        return node ? _channel_take(ch, node) : NULL;
    }

    void* payload = NULL;
    for (;;) {
        mpsc_node_t* node = mpsc_pop(&ch->queue);
        if (node) {
            payload = _channel_take(ch, node);
            break;
        }
        if (atomic_load_explicit(&ch->closed, memory_order_acquire)) {
            break;
        }

        uint64_t remaining = 0;
        if (deadline_ms > 0) {
            uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (now >= deadline_ms) {
                break;
            }
            remaining = deadline_ms - now;
        }

        /* Publish self, then re-check: a sender/close may have raced
         * between the pop above and publishing. */
        atomic_store_explicit(&ch->waiter, &w, memory_order_release);
        if (atomic_load_explicit(&ch->closed, memory_order_acquire)
            || !mpsc_empty(&ch->queue)) {
            waiter_t* expected = &w;
            if (atomic_compare_exchange_strong(
                    &ch->waiter, &expected, NULL)) {
                continue; /* undid our publish: retry the pop */
            }
            /* A waker already claimed us; it will post our tsem -- fall
             * through and consume it below so it is not lost. */
        }

        if (infinite) {
            platform_sem_wait(w.tsem);
            continue; /* woken: retry pop */
        }

        if (platform_sem_timedwait(w.tsem, remaining) == 0) {
            continue; /* a waker handed us a token: retry pop */
        }

        /* Timed out: arbitrate with a waker that may have claimed us. */
        waiter_t* expected = &w;
        if (atomic_compare_exchange_strong(&ch->waiter, &expected, NULL)) {
            break; /* removed ourselves cleanly -> timeout */
        }
        /* A waker claimed us and will post tsem; consume it, retry pop. */
        platform_sem_wait(w.tsem);
    }
    return payload;
}

static void* _channel_recv_impl(xylem_channel_t* ch, uint64_t timeout_ms) {
    if (!ch) {
        xylem_loge("<channel> recv on NULL channel");
        return NULL;
    }

    _channel_ref(ch);

    if (atomic_exchange_explicit(&ch->recv_active, true,
                                 memory_order_acq_rel)) {
        xylem_loge("<channel> concurrent recv violates single-receiver "
                   "contract ch=%p; aborting",
                   (void*)ch);
        abort();
    }

    void* payload;
    if (timeout_ms == 0) {
        /* Non-blocking try: one pop, never block. */
        mpsc_node_t* node = mpsc_pop(&ch->queue);
        payload = node ? _channel_take(ch, node) : NULL;
    } else if (mco_running()) {
        payload = _channel_recv_coro(ch, timeout_ms);
    } else {
        payload = _channel_recv_thread(ch, timeout_ms);
    }

    atomic_store_explicit(&ch->recv_active, false, memory_order_release);
    _channel_unref(ch);
    return payload;
}

static xylem_channel_t* _channel_create(size_t cap) {
    xylem_channel_t* ch =
        (xylem_channel_t*)calloc(1, sizeof(xylem_channel_t));
    if (!ch) {
        return NULL;
    }
    mpsc_init(&ch->queue);
    atomic_init(&ch->waiter, NULL);
    atomic_init(&ch->closed, false);
    atomic_init(&ch->recv_active, false);
    atomic_init(&ch->timed_out, false);
    atomic_init(&ch->refcnt, 1);
    ch->cap = cap;
    atomic_init(&ch->count, 0);
    return ch;
}

xylem_channel_t* xylem_channel_create(void) {
    RUNTIME_REQUIRE_COROUTINE("channel", "xylem_channel_create");
    return _channel_create(0);
}

xylem_channel_t* xylem_channel_create_bounded(size_t cap) {
    RUNTIME_REQUIRE_COROUTINE("channel", "xylem_channel_create_bounded");
    if (cap == 0) {
        xylem_loge("<channel> create_bounded requires cap > 0");
        return NULL;
    }
    return _channel_create(cap);
}

void xylem_channel_close(xylem_channel_t* ch) {
    if (!ch) {
        xylem_loge("<channel> close on NULL channel; aborting");
        abort();
    }
    /* Any-thread / any-context: only flips the flag and wakes the
     * parked receiver; never parks. */
    if (atomic_exchange(&ch->closed, true)) {
        xylem_loge("<channel> double close ch=%p; aborting", (void*)ch);
        abort();
    }
    _channel_wake_receiver(ch);
}

void xylem_channel_destroy(xylem_channel_t* ch) {
    if (!ch) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("channel", "xylem_channel_destroy");

    /* Idempotent -- close() may have set this already. */
    atomic_store(&ch->closed, true);
    _channel_wake_receiver(ch);

    _channel_unref(ch);
}

int xylem_channel_send(xylem_channel_t* ch, void* msg) {
    /* The one sync op allowed from any thread: lock-free push + a single
     * atomic_exchange to wake the receiver. Never parks. */
    if (!ch || !msg) {
        xylem_loge("<channel> send NULL argument ch=%p msg=%p",
                   (void*)ch, msg);
        return -1;
    }

    _channel_ref(ch);

    if (atomic_load_explicit(&ch->closed, memory_order_acquire)) {
        xylem_loge("<channel> send on closed channel ch=%p; aborting",
                   (void*)ch);
        abort();
    }

    /**
     * Bounded: reserve a slot with fetch_add before pushing; back it out
     * on overshoot. Keeps in-flight count <= cap under many producers
     * with no check-then-act race. cap == 0 is unbounded; count is still
     * maintained for xylem_channel_len.
     */
    size_t prev = atomic_fetch_add_explicit(
        &ch->count, 1, memory_order_acq_rel);
    if (ch->cap != 0 && prev >= ch->cap) {
        atomic_fetch_sub_explicit(&ch->count, 1, memory_order_relaxed);
        _channel_unref(ch);
        return XYLEM_CHANNEL_FULL;
    }

    int rc = -1;
    _channel_msg_t* m = (_channel_msg_t*)calloc(1, sizeof(_channel_msg_t));
    if (m) {
        m->payload = msg;
        mpsc_push(&ch->queue, &m->node);
        _channel_wake_receiver(ch);
        rc = 0;
    } else {
        /* Allocation failed: release the slot we reserved above. */
        atomic_fetch_sub_explicit(&ch->count, 1, memory_order_relaxed);
    }

    _channel_unref(ch);
    return rc;
}

void* xylem_channel_recv(xylem_channel_t* ch) {
    return _channel_recv_impl(ch, (uint64_t)-1);
}

void* xylem_channel_recv_timeout(
    xylem_channel_t* ch, uint64_t timeout_ms) {
    return _channel_recv_impl(ch, timeout_ms);
}

size_t xylem_channel_len(xylem_channel_t* ch) {
    if (!ch) {
        return 0;
    }
    return atomic_load_explicit(&ch->count, memory_order_relaxed);
}

size_t xylem_channel_cap(xylem_channel_t* ch) {
    if (!ch) {
        return 0;
    }
    return ch->cap;
}
