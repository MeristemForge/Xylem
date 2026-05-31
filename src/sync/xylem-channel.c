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

#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "container/mpsc.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct _channel_msg_s {
    mpsc_node_t node;
    void*       payload;
} _channel_msg_t;

/* refcnt: 1 for creator (dropped by destroy) + 1 per in-flight call
 *         + 1 per armed deadline timer (dropped by the timer callback
 *           when it runs, or by recv when it cancels a pending fire
 *           before the callback can run). The timer-callback ref is
 *           what lets _channel_deadline_cb touch the channel safely
 *           even if the in-flight recv has already returned and the
 *           creator destroyed the channel concurrently.
 *
 * Like every xylem_ runtime object (tcp/udp/dtls conns all embed an
 * iowait_t / sched_timer_t), a channel must not outlive xylem_run:
 * destroy() releases the cached deadline timer, a scheduler-owned
 * resource. The timer is created lazily on first timed recv and
 * reused across calls -- recv_timeout is a per-packet hot path in
 * DTLS/RUDP, so per-call create/destroy would be pure overhead. */
struct xylem_channel_s {
    mpsc_t             queue;
    _Atomic(mco_coro*) wait_coro;
    _Atomic bool       closed;
    _Atomic int32_t    refcnt;
    sched_timer_t*     deadline_timer; /*< lazily created, reused */
    _Atomic bool       timed_out;
};

typedef struct {
    xylem_channel_t* ch;
    uint64_t         deadline_ms; /*< 0 = no deadline */
} _channel_park_ctx_t;

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

static bool _channel_park_cb(mco_coro* co, void* arg) {
    _channel_park_ctx_t* ctx = (_channel_park_ctx_t*)arg;
    xylem_channel_t*     ch  = ctx->ch;

    /* Exchange detects concurrent recv (single-receiver violation). */
    mco_coro* prev = atomic_exchange(&ch->wait_coro, co);
    if (prev != NULL) {
        xylem_loge(
            "concurrent recv violates single-receiver contract "
            "(ch=%p prev=%p new=%p); aborting",
            (void*)ch,
            (void*)prev,
            (void*)co);
        abort();
    }

    /**
     * Re-check after publishing the wait slot: a sender, close, or
     * the deadline timer may have raced in between the emptiness
     * test in the recv loop and publishing co here. Reclaim the slot
     * and decline the park so the recv loop observes the new state.
     * Peek with mpsc_empty only -- popping and re-pushing would break
     * FIFO ordering.
     */
    if (atomic_load(&ch->closed)
        || atomic_load_explicit(&ch->timed_out, memory_order_acquire)
        || !mpsc_empty(&ch->queue)) {
        mco_coro* expected = co;
        if (atomic_compare_exchange_strong(
                &ch->wait_coro, &expected, NULL)) {
            return false;
        }
    }

    return true;
}

static void _channel_deadline_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    xylem_channel_t* ch = (xylem_channel_t*)ud;
    atomic_store_explicit(&ch->timed_out, true, memory_order_release);
    mco_coro* co = atomic_exchange(&ch->wait_coro, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
    /* Release the ref the recv loop handed us when it armed the
     * timer. Reaching zero here is safe: this callback runs while
     * the scheduler holds its own ref on the timer object, so the
     * sched_timer_destroy inside _channel_unref only drops the
     * creator ref and does not free the timer out from under us. */
    _channel_unref(ch);
}

static void* _channel_recv_impl(xylem_channel_t* ch, uint64_t deadline_ms) {
    if (!ch) {
        return NULL;
    }

    _channel_ref(ch);

    /* Lazily create the per-channel deadline timer on first timed
     * recv and reuse it thereafter. The single-receiver contract
     * means only one coroutine ever drives recv, so no extra
     * synchronisation is needed. The timer is a scheduler-owned
     * resource released by destroy(), so the channel must not
     * outlive xylem_run -- the same rule as every xylem_ object. */
    if (deadline_ms > 0 && !ch->deadline_timer) {
        ch->deadline_timer = sched_timer_create(runtime_get_scheduler());
    }

    atomic_store_explicit(&ch->timed_out, false, memory_order_release);

    void* payload = NULL;
    for (;;) {
        mpsc_node_t* node = mpsc_pop(&ch->queue);
        if (node) {
            _channel_msg_t* m = mpsc_entry(node, _channel_msg_t, node);
            payload = m->payload;
            free(m);
            break;
        }

        if (atomic_load(&ch->closed)) {
            break;
        }

        if (deadline_ms > 0) {
            uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (now >= deadline_ms) {
                break;
            }
            if (ch->deadline_timer) {
                /* The timer callback dereferences ch asynchronously
                 * and on a different worker. Hand it its own ref so
                 * it stays valid even if this recv returns and the
                 * channel is destroyed before the callback runs. The
                 * ref is balanced either by the callback itself, or
                 * by us below when sched_timer_stop cancels a fire
                 * that will therefore never run. */
                _channel_ref(ch);
                sched_timer_start(ch->deadline_timer,
                                  _channel_deadline_cb, ch,
                                  deadline_ms - now, 0);
            }
        }

        _channel_park_ctx_t ctx = { .ch = ch, .deadline_ms = deadline_ms };
        scheduler_park(runtime_get_scheduler(), _channel_park_cb, &ctx);

        if (deadline_ms > 0 && ch->deadline_timer) {
            /* If stop() returns true the pending fire was cancelled
             * and the callback will not run, so we own the ref we
             * handed it above and must release it. If it returns
             * false the callback already ran (or is running) and
             * will release that ref itself. */
            if (sched_timer_stop(ch->deadline_timer)) {
                _channel_unref(ch);
            }
        }

        /* A message that was delivered while we were parked must win
         * over a deadline that fired in the same window: re-attempt
         * the pop before honouring timed_out. Otherwise a sender that
         * dequeued our wait slot just before the timer fired would
         * leave its message stranded in the queue and we would
         * wrongly report a timeout. */
        node = mpsc_pop(&ch->queue);
        if (node) {
            _channel_msg_t* m = mpsc_entry(node, _channel_msg_t, node);
            payload = m->payload;
            free(m);
            break;
        }
        if (atomic_load_explicit(&ch->timed_out, memory_order_acquire)) {
            break;
        }
    }

    _channel_unref(ch);
    return payload;
}

xylem_channel_t* xylem_channel_create(void) {
    xylem_channel_t* ch =
        (xylem_channel_t*)calloc(1, sizeof(xylem_channel_t));
    if (!ch) {
        return NULL;
    }
    mpsc_init(&ch->queue);
    atomic_init(&ch->wait_coro, NULL);
    atomic_init(&ch->closed, false);
    atomic_init(&ch->refcnt, 1);
    return ch;
}

void xylem_channel_close(xylem_channel_t* ch) {
    if (!ch) {
        xylem_loge("close(NULL); aborting");
        abort();
    }

    if (atomic_exchange(&ch->closed, true)) {
        xylem_loge("double close (ch=%p); aborting", (void*)ch);
        abort();
    }

    mco_coro* co = atomic_exchange(&ch->wait_coro, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

void xylem_channel_destroy(xylem_channel_t* ch) {
    if (!ch) {
        return;
    }

    /* Idempotent: close() may have already set this. */
    atomic_store(&ch->closed, true);

    mco_coro* co = atomic_exchange(&ch->wait_coro, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }

    _channel_unref(ch);
}

int xylem_channel_send(xylem_channel_t* ch, void* msg) {
    if (!ch || !msg) {
        return -1;
    }

    _channel_ref(ch);

    if (atomic_load_explicit(&ch->closed, memory_order_acquire)) {
        xylem_loge("send on closed channel (ch=%p); aborting",
                   (void*)ch);
        abort();
    }

    int rc = -1;
    _channel_msg_t* m = (_channel_msg_t*)calloc(1, sizeof(_channel_msg_t));
    if (m) {
        m->payload = msg;
        mpsc_push(&ch->queue, &m->node);

        mco_coro* co = atomic_exchange(&ch->wait_coro, NULL);
        if (co) {
            scheduler_schedule(runtime_get_scheduler(), co);
        }
        rc = 0;
    }

    _channel_unref(ch);
    return rc;
}

void* xylem_channel_recv(xylem_channel_t* ch) {
    return _channel_recv_impl(ch, 0);
}

void* xylem_channel_recv_timeout(
    xylem_channel_t* ch, uint64_t deadline_ms) {
    return _channel_recv_impl(ch, deadline_ms);
}
