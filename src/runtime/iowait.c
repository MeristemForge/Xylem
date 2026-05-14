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

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "runtime.h"
#include "scheduler.h"
#include "thrds.h"

#include "minicoro/minicoro.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Poller ud = (gen, slab-index) packed into uintptr_t. gen rejects stale CQEs. */
#define IOWAIT_INDEX_BITS ((int)(sizeof(uintptr_t) * CHAR_BIT - 16))
#define IOWAIT_INDEX_MASK (((uintptr_t)1 << IOWAIT_INDEX_BITS) - 1)
#define IOWAIT_GEN_SHIFT  IOWAIT_INDEX_BITS

#define IOWAIT_FLAG_CLOSED   ((uint32_t)1)
#define IOWAIT_FLAG_DEADLINE ((uint32_t)2)

typedef struct _iowait_park_s _iowait_park_t;
typedef struct _iowait_dir_s  _iowait_dir_t;

/* Per-direction state (rd/wr symmetric). timer allocated lazily. */
struct _iowait_dir_s {
    iowait_t*                w;
    _Atomic(_iowait_park_t*) park;
    sched_timer_t*           timer;
    _Atomic uint64_t         deadline;
    _Atomic uint32_t         flags;
};

/* On caller's stack; published via dir->park. Wakers claim it with atomic_exchange. */
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

    _iowait_dir_t         rd;
    _iowait_dir_t         wr;

    mtx_t                 arm_lock;

    _Atomic int32_t       refcnt;
    _Atomic uint16_t      gen;
    bool                  registered;
    _Atomic bool          closed;

    iowait_slab_t*        slab;
    uint32_t              slot_index;
};

/* Paged slab: type-stable memory so stale CQEs can be safely rejected by gen check. */
#define IOWAIT_PAGE_SHIFT 8
#define IOWAIT_PAGE_SIZE  (1u << IOWAIT_PAGE_SHIFT)
#define IOWAIT_FREE_END   UINT32_MAX

#define IOWAIT_PAGES_MAX  (sizeof(void*) <= 4 ? 256 : 1024)

_Static_assert(
    (uint64_t)IOWAIT_PAGES_MAX * IOWAIT_PAGE_SIZE <=
        ((uint64_t)1 << (sizeof(uintptr_t) * CHAR_BIT - 16)),
    "slab capacity exceeds addressable index range");

struct iowait_slab_s {
    mtx_t      lock;
    iowait_t*  pages[IOWAIT_PAGES_MAX];
    uint32_t   npages;
    uint32_t   freehead;
};

static inline iowait_t* _iowait_slab_get(
    iowait_slab_t* slab, uint32_t index) {
    uint32_t zi     = index - 1;
    uint32_t page   = zi >> IOWAIT_PAGE_SHIFT;
    uint32_t offset = zi & (IOWAIT_PAGE_SIZE - 1);
    return &slab->pages[page][offset];
}

static inline uint32_t _iowait_free_next(iowait_t* slot) {
    uint32_t v;
    memcpy(&v, slot, sizeof(v));
    return v;
}

static inline void _iowait_free_set_next(iowait_t* slot, uint32_t next) {
    memcpy(slot, &next, sizeof(next));
}

static inline void* _iowait_ud_encode(uint32_t index, uint16_t gen) {
    uintptr_t v = ((uintptr_t)gen << IOWAIT_GEN_SHIFT)
                | ((uintptr_t)index & IOWAIT_INDEX_MASK);
    return (void*)v;
}

static inline uint32_t _iowait_ud_index(void* ud) {
    return (uint32_t)((uintptr_t)ud & IOWAIT_INDEX_MASK);
}

static inline uint16_t _iowait_ud_tag(void* ud) {
    return (uint16_t)((uintptr_t)ud >> IOWAIT_GEN_SHIFT);
}

static iowait_t* _iowait_slab_alloc(
    iowait_slab_t* slab, uint32_t* out_index) {
    mtx_lock(&slab->lock);

    if (slab->freehead != IOWAIT_FREE_END) {
        uint32_t  idx = slab->freehead;
        iowait_t* w   = _iowait_slab_get(slab, idx);
        slab->freehead = _iowait_free_next(w);
        *out_index = idx;
        mtx_unlock(&slab->lock);
        return w;
    }

    if (slab->npages >= IOWAIT_PAGES_MAX) {
        mtx_unlock(&slab->lock);
        return NULL;
    }

    iowait_t* page =
        (iowait_t*)calloc(IOWAIT_PAGE_SIZE, sizeof(iowait_t));
    if (!page) {
        mtx_unlock(&slab->lock);
        return NULL;
    }

    /* +1 so that index 0 is never used: encode(0, 0) == NULL, which
     * the scheduler reserves for the wakeup-fd sentinel. */
    uint32_t base = slab->npages * IOWAIT_PAGE_SIZE + 1;

    for (uint32_t i = 0; i < IOWAIT_PAGE_SIZE; i++) {
        if (mtx_init(&page[i].arm_lock, mtx_plain) != thrd_success) {
            for (uint32_t j = 0; j < i; j++) {
                mtx_destroy(&page[j].arm_lock);
            }
            free(page);
            mtx_unlock(&slab->lock);
            return NULL;
        }
        page[i].slot_index = base + i;
    }

    slab->pages[slab->npages] = page;
    slab->npages++;

    for (uint32_t i = IOWAIT_PAGE_SIZE - 1; i >= 1; i--) {
        _iowait_free_set_next(&page[i], slab->freehead);
        slab->freehead = base + i;
    }

    *out_index = base;
    mtx_unlock(&slab->lock);
    return &page[0];
}

static void _iowait_slab_free(iowait_slab_t* slab, uint32_t index) {
    iowait_t* w = _iowait_slab_get(slab, index);
    mtx_lock(&slab->lock);
    _iowait_free_set_next(w, slab->freehead);
    slab->freehead = index;
    mtx_unlock(&slab->lock);
}

/* Last ref dropped: bump gen, return slot to slab freelist.
 * Timers are kept alive for reuse by the next occupant. */
static void _iowait_retire(iowait_t* w) {
    atomic_fetch_add_explicit(&w->gen, 1, memory_order_release);
    _iowait_slab_free(w->slab, w->slot_index);
}

static void _iowait_ref(iowait_t* w) {
    atomic_fetch_add_explicit(&w->refcnt, 1, memory_order_relaxed);
}

static void _iowait_unref(iowait_t* w) {
    if (atomic_fetch_sub_explicit(&w->refcnt, 1, memory_order_acq_rel) == 1) {
        _iowait_retire(w);
    }
}

/* CAS-bump refcnt only if non-zero, then verify gen matches the CQE's tag. */
static iowait_t* _iowait_tryref(iowait_t* w, uint16_t expected_tag) {
    int32_t cur =
        atomic_load_explicit(&w->refcnt, memory_order_acquire);
    for (;;) {
        if (cur <= 0) {
            return NULL;
        }
        if (atomic_compare_exchange_weak_explicit(
                &w->refcnt,
                &cur,
                cur + 1,
                memory_order_acq_rel,
                memory_order_acquire)) {
            break;
        }
    }

    uint16_t actual =
        atomic_load_explicit(&w->gen, memory_order_acquire);
    if (actual != expected_tag) {
        _iowait_unref(w);
        return NULL;
    }
    return w;
}

static bool _iowait_deadline_passed(uint64_t deadline_ms) {
    return deadline_ms != 0 &&
           xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline_ms;
}


/* ET: register once with RD+WR. LT+oneshot: re-arm with parked directions under lock. */
static void _iowait_arm(iowait_t* w) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return;
    }

    if (PLATFORM_POLLER_TRIGGER_MODE == PLATFORM_POLLER_TRIGGER_ET) {
        /* Fast path: already registered, no need to touch the lock. */
        if (w->registered) {
            return;
        }
    }

    mtx_lock(&w->arm_lock);

    /* Re-check under the lock: iowait_close may have raced in. */
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        mtx_unlock(&w->arm_lock);
        return;
    }

    if (PLATFORM_POLLER_TRIGGER_MODE == PLATFORM_POLLER_TRIGGER_ET) {
        if (!w->registered) {
            w->sqe.op = PLATFORM_POLLER_RW_OP;
            if (platform_poller_add(w->poller, &w->sqe) == 0) {
                w->registered = true;
            }
        }
        mtx_unlock(&w->arm_lock);
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
        mtx_unlock(&w->arm_lock);
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

    mtx_unlock(&w->arm_lock);
}

/* atomic_exchange arbitrates IO/timer/close race; exactly one winner wakes the coro. */
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

static void _iowait_wake_park_batch(
    scheduler_t* sched, runnable_batch_t* batch, _iowait_park_t* p) {
    if (!p) {
        return;
    }
    if (batch->n == batch->cap) {
        scheduler_schedule_batch(sched, batch->coros, batch->n);
        batch->n = 0;
    }
    batch->coros[batch->n++] = p->co;
}

static void _iowait_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    _iowait_dir_t* d = (_iowait_dir_t*)ud;
    iowait_t*      w = d->w;
    _iowait_park_t* p = _iowait_claim(&d->park, IOWAIT_TIMEOUT);
    if (p) {
        _iowait_wake_park(p);
    } else {
        atomic_fetch_or_explicit(
            &d->flags, IOWAIT_FLAG_DEADLINE, memory_order_release);
    }
    /* Drop timer-arm ref; d is unsafe after this (racing destroy may free w). */
    _iowait_unref(w);
}

/* Publish park, arm fd, then re-check close/deadline to catch races. */
static bool _iowait_park_fn(mco_coro* co, void* arg) {
    _iowait_park_t* p = (_iowait_park_t*)arg;
    iowait_t*       w = p->w;
    _iowait_dir_t*  d = p->dir;

    p->co     = co;
    p->result = IOWAIT_READY;

    /* Single-occupancy: exchange detects illegal concurrent park. */
    _iowait_park_t* prev = atomic_exchange_explicit(
        &d->park, p, memory_order_release);
    if (prev != NULL) {
        xylem_loge(
            "iowait: concurrent park on same direction "
            "violates single-reader/-writer contract "
            "(iowait=%p dir=%s prev_park=%p new_park=%p); aborting",
            (void*)w,
            (d == &w->rd) ? "rd" : "wr",
            (void*)prev,
            (void*)p);
        abort();
    }

    _iowait_arm(w);

    /* Check both close and deadline races in one load. */
    uint32_t f = atomic_load_explicit(&d->flags, memory_order_acquire);
    if (f & IOWAIT_FLAG_CLOSED) {
        _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_CLOSED));
    } else if (f & IOWAIT_FLAG_DEADLINE) {
        _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_TIMEOUT));
    }
    return true;
}

/* Each timer arm owns one iowait ref, released in the timeout cb or on rearm cancel. */
static void _iowait_set_deadline(_iowait_dir_t* d, uint64_t deadline_ms) {
    atomic_store_explicit(&d->deadline, deadline_ms, memory_order_release);

    /* Cancel any timer arm still in flight; if we actually caught it
     * before it fired, return the reference that arm owned. */
    if (d->timer && sched_timer_stop(d->timer)) {
        _iowait_unref(d->w);
    }

    if (deadline_ms == 0) {
        return;
    }

    atomic_fetch_and_explicit(
        &d->flags, ~IOWAIT_FLAG_DEADLINE, memory_order_relaxed);

    if (!d->timer) {
        d->timer = sched_timer_create(runtime_get_scheduler());
        if (!d->timer) {
            return;
        }
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    uint64_t in  = (deadline_ms > now) ? (deadline_ms - now) : 0;

    _iowait_ref(d->w);
    sched_timer_start(d->timer, _iowait_timeout_cb, d, in, 0);
}

static iowait_result_t _iowait_wait(iowait_t* w, _iowait_dir_t* d) {
    _iowait_park_t park = {.w = w, .dir = d};
    scheduler_park(runtime_get_scheduler(), _iowait_park_fn, &park);

    return park.result;
}

iowait_slab_t* iowait_slab_create(void) {
    iowait_slab_t* slab =
        (iowait_slab_t*)calloc(1, sizeof(iowait_slab_t));
    if (!slab) {
        return NULL;
    }
    if (mtx_init(&slab->lock, mtx_plain) != thrd_success) {
        free(slab);
        return NULL;
    }
    slab->freehead = IOWAIT_FREE_END;
    return slab;
}

void iowait_slab_destroy(iowait_slab_t* slab) {
    if (!slab) {
        return;
    }
    for (uint32_t p = 0; p < slab->npages; p++) {
        iowait_t* page = slab->pages[p];
        for (uint32_t i = 0; i < IOWAIT_PAGE_SIZE; i++) {
            if (page[i].rd.timer) {
                sched_timer_destroy(page[i].rd.timer);
            }
            if (page[i].wr.timer) {
                sched_timer_destroy(page[i].wr.timer);
            }
            mtx_destroy(&page[i].arm_lock);
        }
        free(page);
    }
    mtx_destroy(&slab->lock);
    free(slab);
}

iowait_t* iowait_create(platform_sock_t fd) {
    iowait_slab_t* slab =
        scheduler_get_iowait_slab(runtime_get_scheduler());
    uint32_t  index;
    iowait_t* w = _iowait_slab_alloc(slab, &index);
    if (!w) {
        return NULL;
    }

    atomic_store_explicit(&w->rd.park, NULL, memory_order_relaxed);
    atomic_store_explicit(&w->wr.park, NULL, memory_order_relaxed);
    atomic_store_explicit(&w->rd.deadline, 0, memory_order_relaxed);
    atomic_store_explicit(&w->wr.deadline, 0, memory_order_relaxed);
    atomic_store_explicit(&w->rd.flags, 0, memory_order_relaxed);
    atomic_store_explicit(&w->wr.flags, 0, memory_order_relaxed);
    atomic_store_explicit(&w->closed, false, memory_order_relaxed);
    w->registered = false;

    w->slab = slab;

    w->poller = runtime_get_poller();
    w->fd     = fd;

    w->sqe.fd = (platform_poller_fd_t)fd;
    w->sqe.op = PLATFORM_POLLER_NO_OP;
    uint16_t gen = atomic_load_explicit(&w->gen, memory_order_relaxed);
    w->sqe.ud = _iowait_ud_encode(index, gen);

    w->rd.w = w;
    w->wr.w = w;

    atomic_store_explicit(&w->refcnt, 1, memory_order_release);
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

    /* Drop poller subscription now so the caller can safely close the fd. */
    mtx_lock(&w->arm_lock);
    if (w->registered) {
        platform_poller_del(w->poller, &w->sqe);
        w->registered = false;
    }
    mtx_unlock(&w->arm_lock);

    atomic_fetch_or_explicit(
        &w->rd.flags, IOWAIT_FLAG_CLOSED, memory_order_release);
    atomic_fetch_or_explicit(
        &w->wr.flags, IOWAIT_FLAG_CLOSED, memory_order_release);

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

void iowait_on_event(
    scheduler_t*      sched,
    int               revents,
    void*             ud,
    runnable_batch_t* batch) {
    uint32_t       index = _iowait_ud_index(ud);
    uint16_t       tag   = _iowait_ud_tag(ud);
    iowait_slab_t* slab  = scheduler_get_iowait_slab(sched);
    iowait_t*      w     = _iowait_tryref(_iowait_slab_get(slab, index), tag);
    if (!w) {
        return;
    }

    if (revents & PLATFORM_POLLER_RD_OP) {
        _iowait_wake_park_batch(
            sched, batch, _iowait_claim(&w->rd.park, IOWAIT_READY));
    }
    if (revents & PLATFORM_POLLER_WR_OP) {
        _iowait_wake_park_batch(
            sched, batch, _iowait_claim(&w->wr.park, IOWAIT_READY));
    }

    /* LT+oneshot: re-arm for still-parked directions. */
    if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
        _iowait_arm(w);
    }

    _iowait_unref(w);
}
