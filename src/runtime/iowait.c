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
#include "c11-threads.h"

#include <stdatomic.h>
#include <stdlib.h>

enum {
    IOWAIT_IDLE    = 0,
    IOWAIT_WAITING = 1,
    IOWAIT_READY   = 2,
};

typedef struct {
    iowait_t* w;
    uint64_t  timeout_ms;
    bool      timed_out;
    bool      closed;
} _iowait_park_t;

struct iowait_s {
    platform_poller_sq_t* poller;
    platform_poller_sqe_t sqe;
    sched_timer_t*        rd_timer;
    sched_timer_t*        wr_timer;
    platform_sock_t       fd;
    mco_coro*             rd_coro;
    mco_coro*             wr_coro;
    _iowait_park_t*       rd_park;
    _iowait_park_t*       wr_park;
    _Atomic int           rd_state;
    _Atomic int           wr_state;
    _Atomic int           refcnt;
    /**
     * Serializes _iowait_arm() across the rd and wr coroutines. Without
     * this, two coroutines racing to arm the same waiter can corrupt
     * w->sqe.op or issue a duplicate poller_add.
     */
    mtx_t                 arm_lock;
    bool                  registered;
    _Atomic bool          closed;
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
    mtx_destroy(&w->arm_lock);
    free(w);
}

static void _iowait_retain(iowait_t* w) {
    atomic_fetch_add_explicit(&w->refcnt, 1, memory_order_relaxed);
}

static void _iowait_release(iowait_t* w) {
    if (atomic_fetch_sub_explicit(
            &w->refcnt, 1, memory_order_acq_rel) == 1) {
        _iowait_do_free(w);
    }
}

static mco_coro* _iowait_wake(_Atomic int* state, mco_coro** coro_slot) {
    int expected = IOWAIT_WAITING;
    if (atomic_compare_exchange_strong(state, &expected, IOWAIT_READY)) {
        mco_coro* co = *coro_slot;
        *coro_slot = NULL;
        return co;
    }
    expected = IOWAIT_IDLE;
    atomic_compare_exchange_strong(state, &expected, IOWAIT_READY);
    return NULL;
}

static void _iowait_arm(iowait_t* w) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return;
    }

    mtx_lock(&w->arm_lock);

    if (PLATFORM_POLLER_TRIGGER_MODE == PLATFORM_POLLER_TRIGGER_ET) {
        /* ET mode: register once with RD+WR, no re-arm needed. */
        if (!w->registered) {
            w->sqe.op = PLATFORM_POLLER_RW_OP;
            if (platform_poller_add(w->poller, &w->sqe) == 0) {
                w->registered = true;
            }
        }
    } else {
        /* LT+oneshot mode: re-arm with current interest after each event. */
        platform_poller_op_t interest = PLATFORM_POLLER_NO_OP;
        if (w->rd_coro) {
            interest |= PLATFORM_POLLER_RD_OP;
        }
        if (w->wr_coro) {
            interest |= PLATFORM_POLLER_WR_OP;
        }
        if (interest == PLATFORM_POLLER_NO_OP) {
            mtx_unlock(&w->arm_lock);
            return;
        }

        w->sqe.op = interest;
        if (!w->registered) {
            if (platform_poller_add(w->poller, &w->sqe) == 0) {
                w->registered = true;
            }
        } else {
            platform_poller_mod(w->poller, &w->sqe);
        }
    }

    mtx_unlock(&w->arm_lock);
}

static void _iowait_rd_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    mco_coro* co = _iowait_wake(&w->rd_state, &w->rd_coro);
    if (co) {
        if (w->rd_park) {
            w->rd_park->timed_out = true;
        }
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

static void _iowait_wr_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    mco_coro* co = _iowait_wake(&w->wr_state, &w->wr_coro);
    if (co) {
        if (w->wr_park) {
            w->wr_park->timed_out = true;
        }
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

/* Park callback for iowait_read — runs after coroutine is fully suspended. */
static bool _iowait_rd_park_fn(mco_coro* co, void* arg) {
    _iowait_park_t* p = (_iowait_park_t*)arg;
    iowait_t* w = p->w;

    w->rd_coro = co;
    w->rd_park = p;
    atomic_store(&w->rd_state, IOWAIT_IDLE);

    _iowait_arm(w);

    if (p->timeout_ms > 0) {
        /* New timer per park — avoids reuse races across concurrent parks. */
        p->timer = sched_timer_create(runtime_get_scheduler());
        if (p->timer) {
            sched_timer_start(p->timer, _iowait_rd_timeout_cb,
                              p, p->timeout_ms, 0);
        }
    }

    int expected = IOWAIT_IDLE;
    if (!atomic_compare_exchange_strong(
            &w->rd_state, &expected, IOWAIT_WAITING)) {
        w->rd_coro = NULL;
        w->rd_park = NULL;
        if (p->timer) {
            sched_timer_destroy(p->timer);
            p->timer = NULL;
        }
        return false;
    }

    /**
     * Close-race check. An iowait_close() that started before we wrote
     * rd_coro saw NULL and skipped the wake; now that rd_coro is visible
     * and we are in WAITING state, re-check `closed`. If set, we must
     * wake ourselves since no one else will: _iowait_arm() already
     * skipped poller registration because it observed closed=true.
     */
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        mco_coro* me = _iowait_wake(&w->rd_state, &w->rd_coro);
        if (me) {
            p->closed = true;
            scheduler_schedule(runtime_get_scheduler(), me);
        }
    }
    return true;
}

/* Park callback for iowait_write — runs after coroutine is fully suspended. */
static bool _iowait_wr_park_fn(mco_coro* co, void* arg) {
    _iowait_park_t* p = (_iowait_park_t*)arg;
    iowait_t* w = p->w;

    w->wr_coro = co;
    w->wr_park = p;
    atomic_store(&w->wr_state, IOWAIT_IDLE);

    _iowait_arm(w);

    if (p->timeout_ms > 0) {
        p->timer = sched_timer_create(runtime_get_scheduler());
        if (p->timer) {
            sched_timer_start(p->timer, _iowait_wr_timeout_cb,
                              p, p->timeout_ms, 0);
        }
    }

    int expected = IOWAIT_IDLE;
    if (!atomic_compare_exchange_strong(
            &w->wr_state, &expected, IOWAIT_WAITING)) {
        w->wr_coro = NULL;
        w->wr_park = NULL;
        if (p->timer) {
            sched_timer_destroy(p->timer);
            p->timer = NULL;
        }
        return false;
    }

    /* See _iowait_rd_park_fn for rationale. */
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        mco_coro* me = _iowait_wake(&w->wr_state, &w->wr_coro);
        if (me) {
            p->closed = true;
            scheduler_schedule(runtime_get_scheduler(), me);
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

    mtx_init(&w->arm_lock, mtx_plain);

    /* Refcount starts at 1 — owned by the creator (released in iowait_destroy). */
    atomic_store_explicit(&w->refcnt, 1, memory_order_relaxed);
    return w;
}

bool iowait_read(iowait_t* w, uint64_t timeout_ms) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return false;
    }

    _iowait_retain(w);

    _iowait_park_t park = {
        .w = w, .timer = NULL, .timeout_ms = timeout_ms,
        .timed_out = false, .closed = false
    };
    scheduler_park(runtime_get_scheduler(), _iowait_rd_park_fn, &park);

    bool ok = !park.closed && !park.timed_out;

    if (!park.closed) {
        atomic_store(&w->rd_state, IOWAIT_IDLE);
        w->rd_coro = NULL;
        w->rd_park = NULL;
    }

    /**
     * Destroy the per-park timer if one was created. sched_timer_destroy
     * bumps seq under timer_lock so a concurrent _sched_process_timers
     * pass that has already dequeued the timer will still invoke the cb
     * once with park->timer==NULL-safe semantics, but the cb will observe
     * rd_state != WAITING (we either already timed out, or the wake
     * path raced us) and do nothing.
     */
    if (park.timer) {
        sched_timer_destroy(park.timer);
    }

    _iowait_release(w);
    return ok;
}

bool iowait_write(iowait_t* w, uint64_t timeout_ms) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return false;
    }

    _iowait_retain(w);

    _iowait_park_t park = {
        .w = w, .timer = NULL, .timeout_ms = timeout_ms,
        .timed_out = false, .closed = false
    };
    scheduler_park(runtime_get_scheduler(), _iowait_wr_park_fn, &park);

    bool ok = !park.closed && !park.timed_out;

    if (!park.closed) {
        atomic_store(&w->wr_state, IOWAIT_IDLE);
        w->wr_coro = NULL;
        w->wr_park = NULL;
    }

    if (park.timer) {
        sched_timer_destroy(park.timer);
    }

    _iowait_release(w);
    return ok;
}

void iowait_close(iowait_t* w) {
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &w->closed, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

    /**
     * Best-effort wake of any waiter that was already visible to us
     * (i.e., finished its park_fn before we CAS'd `closed`). A waiter
     * that is mid-park_fn may not yet have published rd_coro/wr_coro
     * here; those waiters self-rescue by re-checking `closed` after
     * transitioning to WAITING (see _iowait_rd_park_fn).
     */
    scheduler_t* sched = runtime_get_scheduler();
    if (w->rd_coro) {
        if (w->rd_park) {
            w->rd_park->closed = true;
        }
        mco_coro* co = _iowait_wake(&w->rd_state, &w->rd_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }
    if (w->wr_coro) {
        if (w->wr_park) {
            w->wr_park->closed = true;
        }
        mco_coro* co = _iowait_wake(&w->wr_state, &w->wr_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }
}

void iowait_destroy(iowait_t* w) {
    if (!atomic_load_explicit(&w->closed, memory_order_acquire)) {
        iowait_close(w);
    }
    /* Drop the creator's reference; the last holder frees. */
    _iowait_release(w);
}

bool iowait_is_closed(iowait_t* w) {
    return atomic_load_explicit(&w->closed, memory_order_acquire);
}

void iowait_on_event(int revents, void* ud) {
    iowait_t* w = (iowait_t*)ud;
    _iowait_retain(w);

    scheduler_t* sched = runtime_get_scheduler();

    if ((revents & PLATFORM_POLLER_RD_OP)) {
        mco_coro* co = _iowait_wake(&w->rd_state, &w->rd_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }
    if ((revents & PLATFORM_POLLER_WR_OP)) {
        mco_coro* co = _iowait_wake(&w->wr_state, &w->wr_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }

    if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
        _iowait_arm(w);
    }

    _iowait_release(w);
}
