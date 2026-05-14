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

/**
 * Generation-tagged userdata encoding.
 *
 * Every poller ud carries a (gen, slab-index) pair packed into a
 * uintptr_t. On 32-bit platforms the 16-bit index yields 65536
 * maximum concurrent handles; on 64-bit the 48-bit index is
 * effectively unbounded.
 *
 * gen is bumped on every retire; iowait_on_event rejects stale
 * CQEs by comparing the tag against the live gen after tryref.
 */
#define IOWAIT_INDEX_BITS ((int)(sizeof(uintptr_t) * CHAR_BIT - 16))
#define IOWAIT_INDEX_MASK (((uintptr_t)1 << IOWAIT_INDEX_BITS) - 1)
#define IOWAIT_GEN_SHIFT  IOWAIT_INDEX_BITS

#define IOWAIT_FLAG_CLOSED   ((uint32_t)1)
#define IOWAIT_FLAG_DEADLINE ((uint32_t)2)

typedef struct _iowait_park_s _iowait_park_t;
typedef struct _iowait_dir_s  _iowait_dir_t;

/**
 * Per-direction state. Read and write are fully symmetric; bundling
 * them shares helpers between the two slots.
 *
 * `timer` is allocated lazily on the first non-zero deadline and is
 * not safe against concurrent deadline sets on the same direction;
 * the public API documents this constraint.
 */
struct _iowait_dir_s {
    iowait_t*                w;
    _Atomic(_iowait_park_t*) park;
    sched_timer_t*           timer;
    _Atomic uint64_t         deadline;
    _Atomic uint32_t         flags;
};

/**
 * Per-park state kept on the caller's stack. The iowait publishes
 * a pointer to this via dir->park. Wakers race to atomically swap
 * the slot back to NULL; whichever thread succeeds sets `result`
 * and reschedules park->co. Every waker must go through
 * _iowait_claim because the record is only valid for the duration
 * of the scheduler_park call.
 */
struct _iowait_park_s {
    mco_coro*       co;
    iowait_t*       w;
    _iowait_dir_t*  dir;
    iowait_result_t result;
};

/**
 * Central iowait handle.
 *
 *   arm_lock   - serialises "observe parks -> publish sqe -> submit
 *                to poller" in _iowait_arm.
 *   refcnt     - at zero the handle returns to the slab freelist
 *                (memory stays live); stale CQEs are rejected by
 *                the generation check in _iowait_tryref.
 *   gen        - bumped on every retire, mirrored into the ud tag.
 *   slot_index - this slot's index in the owning slab, set once at
 *                first page allocation, stable for the slab's life.
 */
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

/**
 * Per-scheduler paged slab for iowait handles.
 *
 * Keeps retired handles alive in type-stable memory so that stale
 * CQEs can be safely dispatched through iowait_on_event: the slot
 * memory is never freed, and the per-handle generation counter lets
 * the dispatcher reject stale references.
 *
 * Pages are allocated on demand and never freed until slab destroy.
 * The fixed-size pages array avoids realloc so _iowait_slab_get is
 * safe to call lock-free from iowait_on_event.
 */
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

/**
 * Drop the handle's last reference and return it to the slab.
 *
 * At refcnt==0 no timer-arm reference or park reference can still
 * be outstanding, so per-request state can be torn down with plain
 * accesses.
 *
 * Poller un-registration does NOT happen here; iowait_close does it
 * synchronously. Deferring it to retire would race with fd reuse:
 * the caller's close() could let the kernel recycle the fd number,
 * a subsequent create() could hand that number to a fresh
 * subscription, and a belated EPOLL_CTL_DEL would clobber it.
 *
 * After tear-down we bump gen and return the slot to the slab
 * freelist. Any stale CQE still in a worker's batch is rejected
 * by iowait_on_event's gen check after it reacquires a reference
 * via _iowait_tryref. arm_lock stays live across retire; it is
 * torn down only in iowait_slab_destroy().
 */
static void _iowait_retire(iowait_t* w) {
    if (w->rd.timer) {
        sched_timer_destroy(w->rd.timer);
        w->rd.timer = NULL;
    }
    if (w->wr.timer) {
        sched_timer_destroy(w->wr.timer);
        w->wr.timer = NULL;
    }

    /**
     * Bump the generation. Release ordering pairs with the acquire
     * load in _iowait_on_event so a dispatcher that observes the new
     * gen also observes the fact that refcnt just hit zero (and
     * therefore the handle has been retired).
     */
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

/**
 * Speculatively acquire a reference on a possibly-retired handle.
 *
 * Used only by iowait_on_event, which decoded an untrusted ud into
 * (index, tag) and must verify the handle still matches the tag
 * before treating it as live. Two hazards:
 *
 *   1. refcnt may have already hit zero; don't resurrect a dead
 *      handle.
 *   2. The handle may have been recycled to a fresh caller at a
 *      newer generation; don't dispatch the old CQE to the new
 *      generation.
 *
 * The CAS loop handles (1): bump refcnt only if non-zero. After a
 * successful bump, re-read gen and compare against the tag. A
 * mismatch means the handle was retired *and* reused in the
 * window; drop the ref and return NULL. acq_rel on the bump syncs
 * with the retire path's gen++.
 */
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


/**
 * Ensure the fd is registered with the poller.
 *
 * ET: register once with RD+WR; after the first add every park
 * fast-returns. LT+oneshot: re-arm each time with the union of
 * currently-parked directions, since the kernel drops the
 * registration after every event.
 *
 * Called from both the parking worker and the poller thread. On
 * LT+oneshot they would otherwise both compute an op from a stale
 * park snapshot, write the shared sqe, and race into
 * platform_poller_mod -- the later MOD would silently drop a
 * direction the other thread just published. arm_lock serialises
 * the full "observe -> publish -> submit" sequence per handle;
 * cross-fd arms remain fully parallel.
 *
 * ET fast path reads `registered` without the lock because its
 * transition is one-way (false->true), so a stale false just falls
 * through to the locked slow path.
 */
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

/**
 * Single arbitration point for waking a parked coroutine.
 *
 * Up to three sources race to wake the same park: IO event, deadline
 * timer, and iowait_close(). atomic_exchange picks exactly one winner
 * and hands it the park pointer with the cause stamped into `result`.
 * Losers get NULL and must do nothing, guaranteeing the coroutine is
 * never double-scheduled.
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

/**
 * Batch variant: append the parked coroutine to the caller's batch
 * instead of scheduling inline. If the batch is full, flush it
 * first so there is always room. Used by iowait_on_event so the
 * whole poll pass amortises a single runq push + wake.
 */
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
    /**
     * Drop the timer-arm reference taken in _iowait_set_deadline.
     * Must be the last thing we do here: once this ref is released,
     * the iowait (and therefore `d` itself) may be freed by a racing
     * iowait_destroy, so `d` is not safe to touch below this point.
     */
    _iowait_unref(w);
}

/**
 * Park body executed by scheduler_park after the coroutine has
 * saved its context. Publishes the park record, (re-)arms the fd,
 * then guards against two races that can swallow the wakeup:
 *
 *   - iowait_close() running between our slot publish and the yield:
 *     closer's _iowait_claim saw a NULL slot and skipped us.
 *   - Deadline timer firing before we published: _iowait_timeout_cb
 *     had nothing to claim.
 *
 * Both cases re-observe the condition after publishing and self-wake
 * by claiming our own park slot.
 */
static bool _iowait_park_fn(mco_coro* co, void* arg) {
    _iowait_park_t* p = (_iowait_park_t*)arg;
    iowait_t*       w = p->w;
    _iowait_dir_t*  d = p->dir;

    p->co     = co;
    p->result = IOWAIT_READY;

    /**
     * Publish our park record. The slot is single-occupancy by
     * contract (one-reader / one-writer per direction; see iowait.h
     * threading model). Using exchange here gives us a loud fatal if
     * a second coroutine parks on the same direction -- without it
     * the first parker is silently orphaned (no wakeup source will
     * ever find it again) and its handle reference leaks.
     */
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

    /**
     * Single-load race guard (Go's atomicInfo pattern). The flags
     * word packs CLOSED and DEADLINE_EXPIRED so both races are
     * detected in one acquire load instead of two.
     */
    uint32_t f = atomic_load_explicit(&d->flags, memory_order_acquire);
    if (f & IOWAIT_FLAG_CLOSED) {
        _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_CLOSED));
    } else if (f & IOWAIT_FLAG_DEADLINE) {
        _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_TIMEOUT));
    }
    return true;
}

/**
 * Apply a new deadline to a direction.
 *
 * Publishes the deadline word first so a waiter can observe it via
 * the acquire load in _iowait_park_fn / _iowait_wait. The timer is
 * allocated lazily; the header documents the threading constraint
 * on timer mutation.
 *
 * Refcount protocol: each successful timer (re-)arm owns one
 * reference on the iowait, released either by the timeout callback
 * or here when a timer rearm cancels a still-pending fire. Combined
 * with the scheduler keeping timer objects alive across an in-flight
 * fire, the callback always sees a live iowait even if iowait_destroy
 * races in.
 *
 * Known corner case: a deadline of 0 may still result in one late
 * IOWAIT_TIMEOUT delivery if the callback was already dispatched
 * before sched_timer_stop acquired the timer lock. Memory-safe by
 * the refcount protocol, but the extra wake is not eliminated.
 */
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

/**
 * No early-out checks for closed/deadline: the post-publish guards
 * in _iowait_park_fn handle both races. No ref needed: the conn
 * layer holds its own ref around recv/send for the park's duration.
 */
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

    w->rd.timer = NULL;
    w->wr.timer = NULL;
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

    /**
     * Drop the poller subscription synchronously before returning.
     * This is what lets the caller safely close() the fd next:
     * the kernel has already dropped our subscription, so no
     * deferred EPOLL_CTL_DEL can clobber a recycled fd number.
     *
     * arm_lock serialises against concurrent _iowait_arm. After the
     * CAS above every new arm sees closed=true at its entry acquire
     * load and bails before touching the poller, so once we hold
     * the lock any in-flight arm has finished and none will start.
     */
    mtx_lock(&w->arm_lock);
    if (w->registered) {
        platform_poller_del(w->poller, &w->sqe);
        w->registered = false;
    }
    mtx_unlock(&w->arm_lock);

    /**
     * Publish the closed flag into both directions' packed flags
     * word so _iowait_park_fn can detect close-race with a single
     * atomic load (Go's atomicInfo / publishInfo pattern).
     */
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

    /**
     * LT+oneshot: re-arm with whichever directions are still parked.
     * A park that only races into its slot after we read it will take
     * care of its own arm inside _iowait_park_fn. ET pollers stay
     * armed after the initial registration.
     */
    if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
        _iowait_arm(w);
    }

    _iowait_unref(w);
}
