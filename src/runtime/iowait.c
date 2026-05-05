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
#include "sched-timer.h"

#include "minicoro/minicoro.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    IOWAIT_IDLE    = 0,
    IOWAIT_WAITING = 1,
    IOWAIT_READY   = 2,
};

#define IOWAIT_MAGIC_ALIVE 0xA11CE042u
#define IOWAIT_MAGIC_DEAD  0xDEAD0042u

typedef struct {
    iowait_t* w;
    uint64_t  timeout_ms;
    bool      timed_out;
    bool      closed;
} _iowait_park_t;

struct iowait_s {
    _Atomic uint32_t      magic;
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
    bool                  registered;
    bool                  closed;
};

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
    if (w->closed) {
        return;
    }

    platform_poller_op_t interest = PLATFORM_POLLER_NO_OP;
    if (w->rd_coro) {
        interest |= PLATFORM_POLLER_RD_OP;
    }
    if (w->wr_coro) {
        interest |= PLATFORM_POLLER_WR_OP;
    }
    if (interest == PLATFORM_POLLER_NO_OP) {
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

static void _iowait_rd_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    if (w->rd_park) {
        w->rd_park->timed_out = true;
    }
    mco_coro* co = _iowait_wake(&w->rd_state, &w->rd_coro);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

static void _iowait_wr_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    if (w->wr_park) {
        w->wr_park->timed_out = true;
    }
    mco_coro* co = _iowait_wake(&w->wr_state, &w->wr_coro);
    if (co) {
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
        if (!w->rd_timer) {
            w->rd_timer = sched_timer_create(
                scheduler_get_timer_mgr(runtime_get_scheduler()));
        }
        sched_timer_start(w->rd_timer, _iowait_rd_timeout_cb,
                          w, p->timeout_ms, 0);
    }

    int expected = IOWAIT_IDLE;
    if (!atomic_compare_exchange_strong(
            &w->rd_state, &expected, IOWAIT_WAITING)) {
        w->rd_coro = NULL;
        w->rd_park = NULL;
        if (p->timeout_ms > 0 && w->rd_timer) {
            sched_timer_stop(w->rd_timer);
        }
        return false;
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
        if (!w->wr_timer) {
            w->wr_timer = sched_timer_create(
                scheduler_get_timer_mgr(runtime_get_scheduler()));
        }
        sched_timer_start(w->wr_timer, _iowait_wr_timeout_cb,
                          w, p->timeout_ms, 0);
    }

    int expected = IOWAIT_IDLE;
    if (!atomic_compare_exchange_strong(
            &w->wr_state, &expected, IOWAIT_WAITING)) {
        w->wr_coro = NULL;
        w->wr_park = NULL;
        if (p->timeout_ms > 0 && w->wr_timer) {
            sched_timer_stop(w->wr_timer);
        }
        return false;
    }
    return true;
}

iowait_t* iowait_create(platform_sock_t fd) {
    iowait_t* w = (iowait_t*)calloc(1, sizeof(iowait_t));
    if (!w) {
        return NULL;
    }

    atomic_store(&w->magic, IOWAIT_MAGIC_ALIVE);
    w->poller = runtime_get_poller();
    w->fd     = fd;

    w->sqe.fd      = (platform_poller_fd_t)fd;
    w->sqe.ud      = w;
    w->sqe.oneshot  = 1;
    w->sqe.op      = PLATFORM_POLLER_NO_OP;

    return w;
}

bool iowait_read(iowait_t* w, uint64_t timeout_ms) {
    if (w->closed) {
        return false;
    }

    _iowait_park_t park = {
        .w = w, .timeout_ms = timeout_ms,
        .timed_out = false, .closed = false
    };
    scheduler_park(runtime_get_scheduler(), _iowait_rd_park_fn, &park);

    if (park.closed) {
        return false;
    }

    atomic_store(&w->rd_state, IOWAIT_IDLE);
    w->rd_coro = NULL;
    w->rd_park = NULL;

    if (timeout_ms > 0 && w->rd_timer && !park.timed_out) {
        sched_timer_stop(w->rd_timer);
    }

    return !park.timed_out;
}

bool iowait_write(iowait_t* w, uint64_t timeout_ms) {
    if (w->closed) {
        return false;
    }

    _iowait_park_t park = {
        .w = w, .timeout_ms = timeout_ms,
        .timed_out = false, .closed = false
    };
    scheduler_park(runtime_get_scheduler(), _iowait_wr_park_fn, &park);

    if (park.closed) {
        return false;
    }

    atomic_store(&w->wr_state, IOWAIT_IDLE);
    w->wr_coro = NULL;
    w->wr_park = NULL;

    if (timeout_ms > 0 && w->wr_timer && !park.timed_out) {
        sched_timer_stop(w->wr_timer);
    }

    return !park.timed_out;
}

void iowait_close(iowait_t* w) {
    uint32_t m = atomic_load(&w->magic);
    if (m != IOWAIT_MAGIC_ALIVE) {
        fprintf(stderr, "IOWAIT BUG: iowait_close on %s iowait %p (magic=0x%08x)\n",
                m == IOWAIT_MAGIC_DEAD ? "DESTROYED" : "CORRUPT", (void*)w, m);
        assert(0 && "iowait_close: use-after-free or corruption");
    }
    if (w->closed) {
        return;
    }
    w->closed = true;

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
    uint32_t m = atomic_load(&w->magic);
    if (m != IOWAIT_MAGIC_ALIVE) {
        fprintf(stderr, "IOWAIT BUG: iowait_destroy on %s iowait %p (magic=0x%08x)\n",
                m == IOWAIT_MAGIC_DEAD ? "DESTROYED" : "CORRUPT", (void*)w, m);
        assert(0 && "iowait_destroy: double-free or corruption");
    }
    atomic_store(&w->magic, IOWAIT_MAGIC_DEAD);

    if (!w->closed) {
        iowait_close(w);
    }

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

bool iowait_is_closed(iowait_t* w) {
    return w->closed;
}

void iowait_on_event(int revents, void* ud) {
    iowait_t* w = (iowait_t*)ud;
    uint32_t m = atomic_load(&w->magic);
    if (m != IOWAIT_MAGIC_ALIVE) {
        fprintf(stderr, "IOWAIT BUG: iowait_on_event on %s iowait %p (magic=0x%08x)\n",
                m == IOWAIT_MAGIC_DEAD ? "DESTROYED" : "CORRUPT", (void*)w, m);
        assert(0 && "iowait_on_event: stale epoll event for freed iowait");
    }
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

    /* Re-arm for any direction that still has a waiting coro (oneshot). */
    _iowait_arm(w);
}
