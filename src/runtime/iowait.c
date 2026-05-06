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

typedef struct _iowait_park_s _iowait_park_t;
typedef struct _iowait_dir_s  _iowait_dir_t;

/**
 * Per-direction state. Read and write are fully symmetric; bundling
 * them into one struct eliminates the rd/wr code duplication and
 * makes the two iowait slots share a single set of helpers.
 */
struct _iowait_dir_s {
    _Atomic(_iowait_park_t*) park;
    sched_timer_t*           timer;
    _Atomic uint64_t         deadline;
};

/**
 * Per-park state kept on the caller's stack. The iowait publishes a
 * pointer to this via an atomic slot (dir->park). Wakers race to
 * atomically swap the slot back to NULL: whichever thread succeeds
 * sets `result` and reschedules park->co.
 */
struct _iowait_park_s {
    mco_coro*       co;
    iowait_t*       w;
    _iowait_dir_t*  dir;
    iowait_result_t result;
};

struct iowait_s {
    platform_poller_sq_t* poller;
    platform_poller_sqe_t sqe;
    platform_sock_t       fd;

    /**
     * rd/wr state. Each direction owns its park slot, deadline timer
     * (allocated once, reused across deadline changes; 0 deadline means
     * "no deadline" and the timer stays stopped), and deadline value.
     */
    _iowait_dir_t         rd;
    _iowait_dir_t         wr;

    _Atomic int32_t       refcnt;
    bool                  registered;
    _Atomic bool          closed;
};

static void _iowait_do_free(iowait_t* w) {
    if (w->registered) {
        platform_poller_del(w->poller, &w->sqe);
        w->registered = false;
    }
    if (w->rd.timer) {
        sched_timer_destroy(w->rd.timer);
    }
    if (w->wr.timer) {
        sched_timer_destroy(w->wr.timer);
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

static bool _iowait_deadline_passed(uint64_t deadline_ms) {
    return deadline_ms != 0 &&
           xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline_ms;
}

/**
 * Ensure the fd is registered with the poller.
 *
 * Under ET (edge-triggered) the fd is registered once with RD+WR and
 * the kernel notifies on every edge; after that first add we fast-
 * return on every park. Under LT+oneshot the kernel drops the
 * registration after each event, so we re-arm with the union of
 * currently-waiting directions.
 */
static void _iowait_arm(iowait_t* w) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return;
    }

    if (PLATFORM_POLLER_TRIGGER_MODE == PLATFORM_POLLER_TRIGGER_ET) {
        if (w->registered) {
            return;
        }
        w->sqe.op = PLATFORM_POLLER_RW_OP;
        if (platform_poller_add(w->poller, &w->sqe) == 0) {
            w->registered = true;
        }
        return;
    }

    platform_poller_op_t op = PLATFORM_POLLER_NO_OP;
    if (atomic_load_explicit(&w->rd.park, memory_order_acquire)) {
        op |= PLATFORM_POLLER_RD_OP;
    }
    if (atomic_load_explicit(&w->wr.park, memory_order_acquire)) {
        op |= PLATFORM_POLLER_WR_OP;
    }
    if (op == PLATFORM_POLLER_NO_OP) {
        return;
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
 * Single arbitration point for waking a parked coroutine.
 *
 * Up to three sources race to wake the same park: an IO event from the
 * poller, a deadline timer firing, and iowait_close(). They all funnel
 * through here; the atomic_exchange picks exactly one winner and hands
 * it the park pointer with the cause stamped into `result`. Losers get
 * NULL and must do nothing, which is how we guarantee a park is woken
 * at most once and the coroutine is never double-scheduled.
 *
 * The caller is responsible for actually scheduling the returned park
 * (see _iowait_wake_park); this function only transfers ownership.
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

static void _iowait_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    _iowait_dir_t* d = (_iowait_dir_t*)ud;
    _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_TIMEOUT));
}

static bool _iowait_park_fn(mco_coro* co, void* arg) {
    _iowait_park_t* p = (_iowait_park_t*)arg;
    iowait_t*       w = p->w;
    _iowait_dir_t*  d = p->dir;

    p->co     = co;
    p->result = IOWAIT_READY;

    atomic_store_explicit(&d->park, p, memory_order_release);

    _iowait_arm(w);

    /**
     * Close-race check: if iowait_close() ran before we published, it
     * could not wake us. Self-rescue by observing `closed` after the
     * store and claiming our own park.
     */
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_CLOSED));
        return true;
    }

    /**
     * Deadline-race check: the deadline timer may have fired before we
     * published the park, in which case nothing wakes us. Re-check the
     * deadline after publishing; if it has already passed, claim and
     * schedule ourselves with TIMEOUT.
     */
    uint64_t deadline = atomic_load_explicit(
        &d->deadline, memory_order_acquire);
    if (_iowait_deadline_passed(deadline)) {
        _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_TIMEOUT));
    }
    return true;
}

static void _iowait_set_deadline(_iowait_dir_t* d, uint64_t deadline_ms) {
    atomic_store_explicit(&d->deadline, deadline_ms, memory_order_release);

    if (deadline_ms == 0) {
        if (d->timer) {
            sched_timer_stop(d->timer);
        }
        return;
    }

    if (!d->timer) {
        d->timer = sched_timer_create(runtime_get_scheduler());
        if (!d->timer) {
            return;
        }
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    uint64_t in  = (deadline_ms > now) ? (deadline_ms - now) : 0;
    sched_timer_start(d->timer, _iowait_timeout_cb, d, in, 0);
}

static iowait_result_t _iowait_wait(iowait_t* w, _iowait_dir_t* d) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return IOWAIT_CLOSED;
    }

    uint64_t deadline = atomic_load_explicit(
        &d->deadline, memory_order_acquire);
    if (_iowait_deadline_passed(deadline)) {
        return IOWAIT_TIMEOUT;
    }

    _iowait_ref(w);

    _iowait_park_t park = {.w = w, .dir = d};
    scheduler_park(runtime_get_scheduler(), _iowait_park_fn, &park);

    iowait_result_t r = park.result;
    _iowait_unref(w);
    return r;
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
    _iowait_set_deadline(&w->rd, deadline_ms);
}

void iowait_set_wr_deadline(iowait_t* w, uint64_t deadline_ms) {
    _iowait_set_deadline(&w->wr, deadline_ms);
}

iowait_result_t iowait_read(iowait_t* w) {
    return _iowait_wait(w, &w->rd);
}

iowait_result_t iowait_write(iowait_t* w) {
    return _iowait_wait(w, &w->wr);
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
     * close-race check in _iowait_park_fn after publishing.
     */
    _iowait_wake_park(_iowait_claim(&w->rd.park, IOWAIT_CLOSED));
    _iowait_wake_park(_iowait_claim(&w->wr.park, IOWAIT_CLOSED));
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
        _iowait_wake_park(_iowait_claim(&w->rd.park, IOWAIT_READY));
    }
    if (revents & PLATFORM_POLLER_WR_OP) {
        _iowait_wake_park(_iowait_claim(&w->wr.park, IOWAIT_READY));
    }

    /* LT+oneshot: re-arm if anyone is still parked. ET stays armed. */
    if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
        _iowait_arm(w);
    }

    _iowait_unref(w);
}
