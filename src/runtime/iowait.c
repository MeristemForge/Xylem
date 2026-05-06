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

#include "iowait.h"

#include "runtime.h"
#include "scheduler.h"

#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdlib.h>

/**
 * Per-park state kept on the caller's stack. The iowait publishes a
 * pointer to this via an atomic slot (w->rd_park / w->wr_park). Wakers
 * race to atomically swap the slot back to NULL: whichever thread
 * succeeds sets `result` and reschedules park->co.
 *
 * The timer callback disambiguates "fire belongs to current park" from
 * "stale fire from a previous park" by CAS-ing the slot against its
 * own park pointer (passed via the timer's `ud` argument). A stale fire
 * sees a different pointer in the slot (NULL or a newer park) and no-ops.
 */
typedef struct _iowait_park_s {
    mco_coro*       co;
    iowait_t*       w;      /*< back-pointer for timer cb */
    iowait_result_t result; /*< filled by the winning waker */
} _iowait_park_t;

typedef struct {
    _iowait_park_t* park;
    uint64_t        timeout_ms;
} _iowait_park_arg_t;

struct iowait_s {
    platform_poller_sq_t*    poller;
    platform_poller_sqe_t    sqe;
    platform_sock_t          fd;
    _Atomic(_iowait_park_t*) rd_park;
    _Atomic(_iowait_park_t*) wr_park;

    /**
     * Per-direction timer. Lazily created on the first park that has
     * a non-zero timeout. Reused across subsequent parks by calling
     * sched_timer_start() again.
     */
    sched_timer_t*           rd_timer;
    sched_timer_t*           wr_timer;

    _Atomic int32_t          refcnt;
    bool                     registered;
    _Atomic bool             closed;
};

static void _iowait_do_free(iowait_t* w) {
    if (w->registered) {
        platform_poller_del(w->poller, &w->sqe);
        w->registered = false;
    }
    if (w->rd_timer) {
        sched_timer_destroy(w->rd_timer);
    }
    if (w->wr_timer) {
        sched_timer_destroy(w->wr_timer);
    }
    free(w);
}

static void _iowait_ref(iowait_t* w) {
    atomic_fetch_add_explicit(&w->refcnt, 1, memory_order_relaxed);
}

static void _iowait_unref(iowait_t* w) {
    if (atomic_fetch_sub_explicit(&w->refcnt, 1, memory_order_acq_rel) == 1) {
        _iowait_do_free(w);
    }
}

/**
 * Ensure the fd is registered with the poller.
 *
 * Under ET (edge-triggered) the fd is registered once with RD+WR and
 * the kernel notifies on every edge. Under LT+oneshot the kernel drops
 * the registration after each event, so we re-arm with the union of
 * currently-waiting directions.
 */
static void _iowait_arm(iowait_t* w) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return;
    }

    platform_poller_op_t op;
    if (PLATFORM_POLLER_TRIGGER_MODE == PLATFORM_POLLER_TRIGGER_ET) {
        op = PLATFORM_POLLER_RW_OP;
    } else {
        op = PLATFORM_POLLER_NO_OP;
        if (atomic_load_explicit(&w->rd_park, memory_order_acquire)) {
            op |= PLATFORM_POLLER_RD_OP;
        }
        if (atomic_load_explicit(&w->wr_park, memory_order_acquire)) {
            op |= PLATFORM_POLLER_WR_OP;
        }
        if (op == PLATFORM_POLLER_NO_OP) {
            return;
        }
    }

    w->sqe.op = op;
    if (!w->registered) {
        if (platform_poller_add(w->poller, &w->sqe) == 0) {
            w->registered = true;
        }
    } else {
        platform_poller_mod(w->poller, &w->sqe);
    }
}

/**
 * Unconditionally claim the park slot (close / poller paths).
 * Returns the claimed park with its result set, or NULL if the slot
 * was already empty.
 */
static _iowait_park_t* _iowait_claim(
    _Atomic(_iowait_park_t*)* slot, iowait_result_t result) {
    _iowait_park_t* p = atomic_exchange(slot, NULL);
    if (p) {
        p->result = result;
    }
    return p;
}

/**
 * Claim the park slot only if it still points to `expected` (timer path).
 * If a different waker has already replaced the slot, the CAS fails and
 * we return NULL, indicating a stale / late fire.
 */
static _iowait_park_t* _iowait_claim_if(
    _Atomic(_iowait_park_t*)* slot,
    _iowait_park_t*           expected,
    iowait_result_t           result) {
    _iowait_park_t* exp = expected;
    if (atomic_compare_exchange_strong(slot, &exp, NULL)) {
        expected->result = result;
        return expected;
    }
    return NULL;
}

static void _iowait_wake_park(_iowait_park_t* p) {
    if (p) {
        scheduler_schedule(runtime_get_scheduler(), p->co);
    }
}

/**
 * Timer ud is the park pointer that was current when the timer was
 * started. A stale fire (dequeued under an old arming but replaced
 * before the cb runs) still uses the old ud thanks to the scheduler's
 * under-lock ud snapshot, so the CAS-by-park-pointer here correctly
 * rejects it.
 */
static void _iowait_rd_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    _iowait_park_t* expected = (_iowait_park_t*)ud;
    iowait_t*       w        = expected->w;
    _iowait_wake_park(
        _iowait_claim_if(&w->rd_park, expected, IOWAIT_TIMEOUT));
}

static void _iowait_wr_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    _iowait_park_t* expected = (_iowait_park_t*)ud;
    iowait_t*       w        = expected->w;
    _iowait_wake_park(
        _iowait_claim_if(&w->wr_park, expected, IOWAIT_TIMEOUT));
}

static bool _iowait_rd_park_fn(mco_coro* co, void* arg) {
    _iowait_park_arg_t* a = (_iowait_park_arg_t*)arg;
    _iowait_park_t*     p = a->park;
    iowait_t*           w = p->w;

    p->co     = co;
    p->result = IOWAIT_READY;

    /**
     * Start the timer BEFORE publishing the park. If the timer somehow
     * fires before publication (it will not in practice, since timeout
     * is in the future), its cb will see a NULL slot and no-op. After
     * publication, poller / close / timer all race on the slot via CAS.
     */
    if (a->timeout_ms > 0) {
        if (!w->rd_timer) {
            w->rd_timer = sched_timer_create(runtime_get_scheduler());
        }
        if (w->rd_timer) {
            sched_timer_start(
                w->rd_timer, _iowait_rd_timeout_cb, p, a->timeout_ms, 0);
        }
    }

    atomic_store_explicit(&w->rd_park, p, memory_order_release);

    _iowait_arm(w);

    /**
     * Close-race check. iowait_close() may have completed before we
     * published the park. If closed is set and the slot still holds
     * our park, claim it with IOWAIT_CLOSED and self-schedule.
     */
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        _iowait_park_t* self
            = _iowait_claim_if(&w->rd_park, p, IOWAIT_CLOSED);
        if (self) {
            scheduler_schedule(runtime_get_scheduler(), self->co);
        }
    }
    return true;
}

static bool _iowait_wr_park_fn(mco_coro* co, void* arg) {
    _iowait_park_arg_t* a = (_iowait_park_arg_t*)arg;
    _iowait_park_t*     p = a->park;
    iowait_t*           w = p->w;

    p->co     = co;
    p->result = IOWAIT_READY;

    if (a->timeout_ms > 0) {
        if (!w->wr_timer) {
            w->wr_timer = sched_timer_create(runtime_get_scheduler());
        }
        if (w->wr_timer) {
            sched_timer_start(
                w->wr_timer, _iowait_wr_timeout_cb, p, a->timeout_ms, 0);
        }
    }

    atomic_store_explicit(&w->wr_park, p, memory_order_release);

    _iowait_arm(w);

    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        _iowait_park_t* self
            = _iowait_claim_if(&w->wr_park, p, IOWAIT_CLOSED);
        if (self) {
            scheduler_schedule(runtime_get_scheduler(), self->co);
        }
    }
    return true;
}

iowait_t* iowait_create(platform_sock_t fd) {
    iowait_t* w = (iowait_t*)calloc(1, sizeof(iowait_t));
    if (!w) {
        return NULL;
    }

    w->poller = runtime_get_poller();
    w->fd     = fd;

    w->sqe.fd = (platform_poller_fd_t)fd;
    w->sqe.ud = w;
    w->sqe.op = PLATFORM_POLLER_NO_OP;

    atomic_store_explicit(&w->refcnt, 1, memory_order_relaxed);
    return w;
}

iowait_result_t iowait_read(iowait_t* w, uint64_t timeout_ms) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return IOWAIT_CLOSED;
    }

    _iowait_ref(w);

    _iowait_park_t     park = {.w = w};
    _iowait_park_arg_t arg  = {.park = &park, .timeout_ms = timeout_ms};
    scheduler_park(runtime_get_scheduler(), _iowait_rd_park_fn, &arg);

    /**
     * Stop the timer if we did not time out. Best-effort: the scheduler
     * bumps the timer's seq in stop(), but a fire that was already
     * dequeued before we stopped will still invoke the cb. Safe,
     * because the cb's CAS-against-park-pointer rejects any stale fire
     * (our park is no longer in the slot).
     */
    if (timeout_ms > 0 && w->rd_timer && park.result != IOWAIT_TIMEOUT) {
        sched_timer_stop(w->rd_timer);
    }

    iowait_result_t r = park.result;
    _iowait_unref(w);
    return r;
}

iowait_result_t iowait_write(iowait_t* w, uint64_t timeout_ms) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return IOWAIT_CLOSED;
    }

    _iowait_ref(w);

    _iowait_park_t     park = {.w = w};
    _iowait_park_arg_t arg  = {.park = &park, .timeout_ms = timeout_ms};
    scheduler_park(runtime_get_scheduler(), _iowait_wr_park_fn, &arg);

    if (timeout_ms > 0 && w->wr_timer && park.result != IOWAIT_TIMEOUT) {
        sched_timer_stop(w->wr_timer);
    }

    iowait_result_t r = park.result;
    _iowait_unref(w);
    return r;
}

void iowait_close(iowait_t* w) {
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &w->closed,
            &expected,
            true,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return;
    }

    /**
     * Wake any waiter already visible in its park slot. A waiter still
     * mid-park_fn may not have published yet; it self-wakes when it
     * re-checks `closed` after publishing (see *_park_fn).
     */
    _iowait_wake_park(_iowait_claim(&w->rd_park, IOWAIT_CLOSED));
    _iowait_wake_park(_iowait_claim(&w->wr_park, IOWAIT_CLOSED));
}

void iowait_destroy(iowait_t* w) {
    if (!w) {
        return;
    }
    if (!atomic_load_explicit(&w->closed, memory_order_acquire)) {
        iowait_close(w);
    }
    _iowait_unref(w);
}

bool iowait_is_closed(iowait_t* w) {
    return atomic_load_explicit(&w->closed, memory_order_acquire);
}

void iowait_on_event(int revents, void* ud) {
    iowait_t* w = (iowait_t*)ud;
    _iowait_ref(w);

    if (revents & PLATFORM_POLLER_RD_OP) {
        _iowait_wake_park(_iowait_claim(&w->rd_park, IOWAIT_READY));
    }
    if (revents & PLATFORM_POLLER_WR_OP) {
        _iowait_wake_park(_iowait_claim(&w->wr_park, IOWAIT_READY));
    }

    /* LT+oneshot: re-arm if any direction is still parked. ET stays armed. */
    if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
        _iowait_arm(w);
    }

    _iowait_unref(w);
}
