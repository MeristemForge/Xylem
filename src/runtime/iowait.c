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

#include "xylem/xylem-utils.h"

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
 */
typedef struct _iowait_park_s {
    mco_coro*       co;
    iowait_t*       w;
    iowait_result_t result;
} _iowait_park_t;

struct iowait_s {
    platform_poller_sq_t*    poller;
    platform_poller_sqe_t    sqe;
    platform_sock_t          fd;

    _Atomic(_iowait_park_t*) rd_park;
    _Atomic(_iowait_park_t*) wr_park;

    /**
     * Per-direction deadline timer. Allocated once at create time and
     * reused across deadline changes. A deadline of 0 means "no
     * deadline" and the timer is left stopped.
     */
    sched_timer_t*           rd_timer;
    sched_timer_t*           wr_timer;
    _Atomic uint64_t         rd_deadline;
    _Atomic uint64_t         wr_deadline;

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
 * Unconditionally claim the park slot. Returns the claimed park with
 * its result set, or NULL if the slot was already empty.
 */
static _iowait_park_t* _iowait_claim(
    _Atomic(_iowait_park_t*)* slot, iowait_result_t result) {
    _iowait_park_t* p = atomic_exchange(slot, NULL);
    if (p) {
        p->result = result;
    }
    return p;
}

static void _iowait_wake_park(_iowait_park_t* p) {
    if (p) {
        scheduler_schedule(runtime_get_scheduler(), p->co);
    }
}

/* Timer expiry: claim the current parked coroutine (if any) with TIMEOUT. */
static void _iowait_rd_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    _iowait_wake_park(_iowait_claim(&w->rd_park, IOWAIT_TIMEOUT));
}

static void _iowait_wr_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    _iowait_wake_park(_iowait_claim(&w->wr_park, IOWAIT_TIMEOUT));
}

typedef struct {
    _iowait_park_t* park;
} _iowait_park_arg_t;

static bool _iowait_rd_park_fn(mco_coro* co, void* arg) {
    _iowait_park_arg_t* a = (_iowait_park_arg_t*)arg;
    _iowait_park_t*     p = a->park;
    iowait_t*           w = p->w;

    p->co     = co;
    p->result = IOWAIT_READY;

    atomic_store_explicit(&w->rd_park, p, memory_order_release);

    _iowait_arm(w);

    /**
     * Close-race check: if iowait_close() ran before we published, it
     * could not wake us. Self-rescue by observing `closed` after the
     * store and claiming our own park.
     */
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        _iowait_park_t* self = _iowait_claim(&w->rd_park, IOWAIT_CLOSED);
        if (self) {
            scheduler_schedule(runtime_get_scheduler(), self->co);
        }
        return true;
    }

    /**
     * Deadline-race check: the deadline timer may have fired before we
     * published the park, in which case nothing wakes us. Re-check the
     * deadline after publishing; if it has already passed, claim and
     * schedule ourselves with TIMEOUT.
     */
    uint64_t deadline = atomic_load_explicit(
        &w->rd_deadline, memory_order_acquire);
    if (deadline != 0 &&
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline) {
        _iowait_park_t* self = _iowait_claim(&w->rd_park, IOWAIT_TIMEOUT);
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

    atomic_store_explicit(&w->wr_park, p, memory_order_release);

    _iowait_arm(w);

    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        _iowait_park_t* self = _iowait_claim(&w->wr_park, IOWAIT_CLOSED);
        if (self) {
            scheduler_schedule(runtime_get_scheduler(), self->co);
        }
        return true;
    }

    uint64_t deadline = atomic_load_explicit(
        &w->wr_deadline, memory_order_acquire);
    if (deadline != 0 &&
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline) {
        _iowait_park_t* self = _iowait_claim(&w->wr_park, IOWAIT_TIMEOUT);
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

void iowait_set_rd_deadline(iowait_t* w, uint64_t deadline_ms) {
    atomic_store_explicit(&w->rd_deadline, deadline_ms, memory_order_release);

    if (deadline_ms == 0) {
        if (w->rd_timer) {
            sched_timer_stop(w->rd_timer);
        }
        return;
    }

    if (!w->rd_timer) {
        w->rd_timer = sched_timer_create(runtime_get_scheduler());
        if (!w->rd_timer) {
            return;
        }
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    uint64_t in  = (deadline_ms > now) ? (deadline_ms - now) : 0;
    sched_timer_start(w->rd_timer, _iowait_rd_timeout_cb, w, in, 0);
}

void iowait_set_wr_deadline(iowait_t* w, uint64_t deadline_ms) {
    atomic_store_explicit(&w->wr_deadline, deadline_ms, memory_order_release);

    if (deadline_ms == 0) {
        if (w->wr_timer) {
            sched_timer_stop(w->wr_timer);
        }
        return;
    }

    if (!w->wr_timer) {
        w->wr_timer = sched_timer_create(runtime_get_scheduler());
        if (!w->wr_timer) {
            return;
        }
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    uint64_t in  = (deadline_ms > now) ? (deadline_ms - now) : 0;
    sched_timer_start(w->wr_timer, _iowait_wr_timeout_cb, w, in, 0);
}

iowait_result_t iowait_read(iowait_t* w) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return IOWAIT_CLOSED;
    }

    uint64_t deadline = atomic_load_explicit(
        &w->rd_deadline, memory_order_acquire);
    if (deadline != 0 &&
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline) {
        return IOWAIT_TIMEOUT;
    }

    _iowait_ref(w);

    _iowait_park_t     park = {.w = w};
    _iowait_park_arg_t arg  = {.park = &park};
    scheduler_park(runtime_get_scheduler(), _iowait_rd_park_fn, &arg);

    iowait_result_t r = park.result;
    _iowait_unref(w);
    return r;
}

iowait_result_t iowait_write(iowait_t* w) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return IOWAIT_CLOSED;
    }

    uint64_t deadline = atomic_load_explicit(
        &w->wr_deadline, memory_order_acquire);
    if (deadline != 0 &&
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline) {
        return IOWAIT_TIMEOUT;
    }

    _iowait_ref(w);

    _iowait_park_t     park = {.w = w};
    _iowait_park_arg_t arg  = {.park = &park};
    scheduler_park(runtime_get_scheduler(), _iowait_wr_park_fn, &arg);

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
     * mid-park_fn may not have published yet; it self-wakes via the
     * close-race check in *_park_fn after publishing.
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

    /* LT+oneshot: re-arm if anyone is still parked. ET stays armed. */
    if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
        _iowait_arm(w);
    }

    _iowait_unref(w);
}
