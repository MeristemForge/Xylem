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
#include "xylem/xylem-threads.h"
#include "xylem/xylem-utils.h"

#include "runtime/precond.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "container/mpsc.h"
#include "sync/tls-wake.h"

#include "runtime/minicoro/minicoro.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct _channel_msg_s {
    mpsc_node_t node;
    void*       payload;
} _channel_msg_t;

/**
 * The single receiver, parked while it waits. A coroutine records its
 * scheduler (co is filled by the park callback, post-suspend); an OS
 * thread records its per-thread futex wake. Lives on the recv() stack
 * frame; the waker takes a by-value copy under the slot exchange, so it
 * never dereferences the frame after the receiver may have resumed (the
 * thread wake object is thread-local and outlives the frame).
 */
typedef enum _channel_kind_e {
    CHANNEL_WAITER_CO,
    CHANNEL_WAITER_THR,
} _channel_kind_t;

typedef struct _channel_waiter_s {
    _channel_kind_t kind;
    mco_coro*       co;    /* CHANNEL_WAITER_CO  */
    scheduler_t*    sched; /* CHANNEL_WAITER_CO  */
    tls_wake_t*     wake;  /* CHANNEL_WAITER_THR */
} _channel_waiter_t;

/**
 * MPSC channel, lock-free data path.
 *
 * Messages live in an intrusive MPSC queue. The single receiver (a
 * coroutine or an OS thread) publishes itself into the atomic `waiter`
 * slot when it must block; a sender, close(), or a recv deadline timer
 * claims the wakeup with one atomic_exchange on that slot, so exactly
 * one resumes the receiver and none double-wakes. The waiter record is
 * per wait; a thread waiter blocks on its per-thread futex wake
 * (tls-wake) and a coroutine waiter is rescheduled. Timed coroutine
 * receives use a per-call recv op, so a stale timer callback can only
 * mark and wake the waiter it was armed for.
 *
 * refcnt keeps the channel alive across a concurrent destroy: the
 * creator holds one, every in-flight send/recv holds one, and an armed
 * recv deadline timer holds one (so its callback can run after recv returns).
 * A channel must not outlive xylem_run (the timer is scheduler-owned).
 *
 * count backs xylem_channel_len; the capacity gate is the fetch_add
 * reservation in send.
 */
struct xylem_channel_s {
    mpsc_t                      queue;
    _Atomic(_channel_waiter_t*) waiter; /* single receiver, NULL if none  */
    _Atomic bool                closed;
    _Atomic bool                recv_in_progress; /* waiter only means parked */
    _Atomic int32_t             refcnt;
    size_t                      cap;   /* 0 = unbounded; immutable          */
    _Atomic size_t              count; /* queued messages, for len()        */
};

typedef struct _channel_recv_op_s {
    xylem_channel_t*       ch;
    _channel_waiter_t      waiter;
    scheduler_timer_t*     timer;
    _Atomic bool           deadline_fired;
    _Atomic int32_t        refcnt;
} _channel_recv_op_t;

/* Wake a claimed receiver by kind: reschedule a coroutine, signal a thread. */
static void _channel_wake(_channel_waiter_t w) {
    if (w.kind == CHANNEL_WAITER_CO) {
        scheduler_schedule(w.sched, w.co);
    } else {
        tls_wake_signal(w.wake);
    }
}

static inline void _channel_ref(xylem_channel_t* ch) {
    atomic_fetch_add(&ch->refcnt, 1);
}

static bool _channel_is_closed(xylem_channel_t* ch) {
    return atomic_load(&ch->closed);
}

static void _channel_wait_link(void) {
    if (mco_running()) {
        runtime_yield_credit();
    } else {
        thrd_yield();
    }
}

static void _channel_unref(xylem_channel_t* ch) {
    if (atomic_fetch_sub(&ch->refcnt, 1) != 1) {
        return;
    }
    mpsc_node_t* node;
    while ((node = mpsc_pop(&ch->queue)) != NULL) {
        _channel_msg_t* msg = mpsc_entry(node, _channel_msg_t, node);
        free(msg);
    }
    free(ch);
}

static void _channel_consume_credit(uint32_t cost) {
    if (runtime_consume_credit(cost)) {
        runtime_yield_credit();
    }
}

/**
 * Wake the parked receiver, if any (used by send, close, and the timer).
 * The atomic_exchange grants one caller exclusive ownership of the wake:
 * the loser sees NULL, and the woken party's stack waiter stays valid
 * until it is resumed exactly once.
 */
static void _channel_wake_receiver(xylem_channel_t* ch) {
    _channel_waiter_t* w = atomic_exchange(&ch->waiter, NULL);
    if (w) {
        _channel_wake(*w);
    }
}

/* Coroutine receiver: park on a stack waiter, lock-free. */

typedef struct _channel_park_ctx_s {
    xylem_channel_t*   ch;
    _channel_waiter_t* w;
    _Atomic bool*      deadline_fired;
} _channel_park_ctx_t;

static bool _channel_park_cb(mco_coro* co, void* arg) {
    _channel_park_ctx_t* ctx = (_channel_park_ctx_t*)arg;
    xylem_channel_t*     ch  = ctx->ch;

    ctx->w->co = co;
    atomic_store(&ch->waiter, ctx->w);

    /**
     * A sender / close / timer may have raced between the emptiness
     * test and publishing the waiter. The default atomic publish above
     * orders this re-check against sender wakeups. Decline park so the recv loop
     * retries. Peek only -- pop+re-push would break FIFO.
     */
    if (_channel_is_closed(ch)
        || (ctx->deadline_fired
            && atomic_load(ctx->deadline_fired))
        || mpsc_can_pop(&ch->queue)) {
        _channel_waiter_t* expected = ctx->w;
        if (atomic_compare_exchange_strong(&ch->waiter, &expected, NULL)) {
            return false;
        }
    }
    return true;
}

static void _channel_recv_op_ref(_channel_recv_op_t* op) {
    atomic_fetch_add(&op->refcnt, 1);
}

static void _channel_recv_op_unref(_channel_recv_op_t* op) {
    if (atomic_fetch_sub(&op->refcnt, 1) != 1) {
        return;
    }
    if (op->timer) {
        scheduler_timer_destroy(op->timer);
    }
    free(op);
}

static _channel_recv_op_t* _channel_recv_op_create(xylem_channel_t* ch) {
    _channel_recv_op_t* op =
        (_channel_recv_op_t*)calloc(1, sizeof(_channel_recv_op_t));
    if (!op) {
        return NULL;
    }
    op->timer = scheduler_timer_create(runtime_get_scheduler());
    if (!op->timer) {
        free(op);
        return NULL;
    }
    op->ch = ch;
    op->waiter.kind  = CHANNEL_WAITER_CO;
    op->waiter.co    = NULL;
    op->waiter.sched = runtime_get_scheduler();
    atomic_init(&op->deadline_fired, false);
    atomic_init(&op->refcnt, 1);
    return op;
}

static void _channel_recv_timeout_cb(scheduler_timer_t* timer, void* ud) {
    (void)timer;
    _channel_recv_op_t* op = (_channel_recv_op_t*)ud;
    xylem_channel_t*    ch = op->ch;
    atomic_store(&op->deadline_fired, true);

    _channel_waiter_t* expected = &op->waiter;
    if (atomic_compare_exchange_strong(&ch->waiter, &expected, NULL)) {
        _channel_wake(op->waiter);
    }

    _channel_recv_op_unref(op);
    _channel_unref(ch);
}

static void* _channel_take(xylem_channel_t* ch, mpsc_node_t* node) {
    _channel_msg_t* m       = mpsc_entry(node, _channel_msg_t, node);
    void*           payload = m->payload;
    free(m);
    atomic_fetch_sub(&ch->count, 1);
    return payload;
}

/* Non-blocking pop: head message, or NULL when the queue is empty. */
static void* _channel_try_pop(xylem_channel_t* ch) {
    mpsc_node_t* node = mpsc_pop(&ch->queue);
    return node ? _channel_take(ch, node) : NULL;
}

static void* _channel_recv_coro(xylem_channel_t* ch, uint64_t timeout_ms) {
    bool infinite = (timeout_ms == (uint64_t)-1);

    _channel_recv_op_t* op = NULL;
    if (!infinite) {
        op = _channel_recv_op_create(ch);
        if (!op) {
            return _channel_try_pop(ch);
        }
    }
    uint64_t deadline_ms = infinite
        ? 0
        : xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;

    _channel_waiter_t w;
    w.kind  = CHANNEL_WAITER_CO;
    w.co    = NULL;
    w.sched = runtime_get_scheduler();
    _channel_waiter_t* waiter = op ? &op->waiter : &w;

    void* payload = NULL;
    for (;;) {
        mpsc_node_t* node = mpsc_pop(&ch->queue);
        if (node) {
            payload = _channel_take(ch, node);
            break;
        }
        if (_channel_is_closed(ch)) {
            if (atomic_load(&ch->count) == 0) {
                break;
            }
            _channel_wait_link();
            continue;
        }

        if (deadline_ms > 0) {
            uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (now >= deadline_ms) {
                break;
            }
            if (op) {
                /* Refs for the timer cb; balanced by it, or by us on stop(). */
                _channel_ref(ch);
                _channel_recv_op_ref(op);
                if (scheduler_timer_start(
                        op->timer,
                        _channel_recv_timeout_cb,
                        op,
                        deadline_ms - now,
                        0) != 0) {
                    _channel_recv_op_unref(op);
                    _channel_unref(ch);
                    break;
                }
            }
        }

        _channel_park_ctx_t ctx = {
            .ch = ch,
            .w = waiter,
            .deadline_fired = op ? &op->deadline_fired : NULL,
        };
        scheduler_park(runtime_get_scheduler(), _channel_park_cb, &ctx);

        if (deadline_ms > 0 && op) {
            /* stop() true => callback will not run, we own its refs. */
            if (scheduler_timer_stop(op->timer)) {
                _channel_recv_op_unref(op);
                _channel_unref(ch);
            }
        }

        /* Data wins over timeout: don't strand a message racing the deadline. */
        node = mpsc_pop(&ch->queue);
        if (node) {
            payload = _channel_take(ch, node);
            break;
        }
        if (op && atomic_load(&op->deadline_fired)) {
            break;
        }
    }
    if (op) {
        _channel_recv_op_unref(op);
    }
    return payload;
}

/* OS-thread receiver: block on the per-thread futex wake, lock-free. */

static void* _channel_recv_thread(xylem_channel_t* ch, uint64_t timeout_ms) {
    bool infinite = (timeout_ms == (uint64_t)-1);
    uint64_t deadline_ms = infinite
        ? 0
        : xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;

    _channel_waiter_t w;
    w.kind = CHANNEL_WAITER_THR;
    w.wake = tls_wake_self();
    if (!w.wake) {
        /* No wake object (OOM): best-effort single pop, never block. */
        return _channel_try_pop(ch);
    }

    void* payload = NULL;
    for (;;) {
        mpsc_node_t* node = mpsc_pop(&ch->queue);
        if (node) {
            payload = _channel_take(ch, node);
            break;
        }
        if (_channel_is_closed(ch)) {
            if (atomic_load(&ch->count) == 0) {
                break;
            }
            _channel_wait_link();
            continue;
        }

        uint64_t remaining = 0;
        if (deadline_ms > 0) {
            uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (now >= deadline_ms) {
                break;
            }
            remaining = deadline_ms - now;
        }

        /* Publish self, then re-check for a send/close racing the pop. */
        atomic_store(&ch->waiter, &w);
        if (_channel_is_closed(ch)
            || mpsc_can_pop(&ch->queue)) {
            _channel_waiter_t* expected = &w;
            if (atomic_compare_exchange_strong(&ch->waiter, &expected, NULL)) {
                continue; /* undid our publish: retry the pop */
            }
            /* A waker claimed us; fall through to consume its wake token. */
        }

        if (infinite) {
            tls_wake_wait(w.wake);
            continue; /* woken: retry pop */
        }

        if (tls_wake_timedwait(w.wake, remaining)) {
            continue; /* a waker handed us a token: retry pop */
        }

        /* Timed out: arbitrate with a waker that may have claimed us. */
        _channel_waiter_t* expected = &w;
        if (atomic_compare_exchange_strong(&ch->waiter, &expected, NULL)) {
            break; /* removed ourselves cleanly -> timeout */
        }
        /* A waker claimed us and will signal; consume the token, retry pop. */
        tls_wake_wait(w.wake);
    }
    return payload;
}

static void* _channel_recv_impl(xylem_channel_t* ch, uint64_t timeout_ms) {
    if (!ch) {
        xylem_loge("<channel> recv on NULL channel");
        return NULL;
    }

    _channel_ref(ch);

    if (atomic_exchange(&ch->recv_in_progress, true)) {
        xylem_loge("<channel> concurrent recv violates single-receiver "
                   "contract ch=%p; aborting",
                   (void*)ch);
        abort();
    }

    void* payload;
    if (timeout_ms == 0) {
        payload = _channel_try_pop(ch); /* non-blocking try */
    } else if (mco_running()) {
        payload = _channel_recv_coro(ch, timeout_ms);
    } else {
        payload = _channel_recv_thread(ch, timeout_ms);
    }

    atomic_store(&ch->recv_in_progress, false);
    _channel_unref(ch);
    if (payload) {
        _channel_consume_credit(1);
    }
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
    atomic_init(&ch->recv_in_progress, false);
    atomic_init(&ch->refcnt, 1);
    ch->cap = cap;
    atomic_init(&ch->count, 0);
    return ch;
}

xylem_channel_t* xylem_channel_create(size_t cap) {
    RUNTIME_REQUIRE_COROUTINE("channel", "xylem_channel_create");
    return _channel_create(cap);
}

void xylem_channel_close(xylem_channel_t* ch) {
    if (!ch) {
        xylem_loge("<channel> close on NULL channel; aborting");
        abort();
    }
    /* Any context: flip the flag and wake the receiver; never parks. */
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
    /* The one op callable from any thread: lock-free push + one wake exchange. */
    if (!ch || !msg) {
        xylem_loge("<channel> send NULL argument ch=%p msg=%p",
                   (void*)ch, msg);
        return -1;
    }

    _channel_ref(ch);

    if (_channel_is_closed(ch)) {
        xylem_loge("<channel> send on closed channel ch=%p; aborting",
                   (void*)ch);
        abort();
    }

    /**
     * Bounded (cap > 0): reserve a slot with fetch_add before pushing;
     * back it out on overshoot. Keeps in-flight count <= cap under many
     * producers with no check-then-act race. cap == 0 is unbounded; the
     * count is still maintained so xylem_channel_len works.
     */
    size_t prev = atomic_fetch_add(&ch->count, 1);
    if (ch->cap != 0 && prev >= ch->cap) {
        atomic_fetch_sub(&ch->count, 1);
        _channel_unref(ch);
        return INT_MAX;
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
        atomic_fetch_sub(&ch->count, 1);
    }

    _channel_unref(ch);
    if (rc == 0) {
        _channel_consume_credit(1);
    }
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
    return atomic_load(&ch->count);
}

size_t xylem_channel_cap(xylem_channel_t* ch) {
    if (!ch) {
        return 0;
    }
    return ch->cap;
}
