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
#include "xylem/xylem-threads.h"

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

#define IOWAIT_PAGE_SHIFT 8
#define IOWAIT_PAGE_SIZE  (1u << IOWAIT_PAGE_SHIFT)
#define IOWAIT_FREE_END   UINT32_MAX
#define IOWAIT_PAGES_MAX  (sizeof(void*) <= 4 ? 256 : 4096)

#define IOWAIT_READY_BIT 1u

_Static_assert(
    (uint64_t)IOWAIT_PAGES_MAX * IOWAIT_PAGE_SIZE <=
        ((uint64_t)1 << (sizeof(uintptr_t) * CHAR_BIT - 16)),
    "slab capacity exceeds addressable index range");

typedef struct _iowait_dir_s _iowait_dir_t;

struct _iowait_dir_s {
    iowait_t*          w;
    _Atomic(mco_coro*) park;
    _Atomic uint32_t   state; /* bit0=ready, bits1..31=tick */
    _Atomic bool       active;
    scheduler_timer_t*     timer;
    _Atomic uint64_t   deadline;
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
    _Atomic bool          registered;
    _Atomic int           interest;
    _Atomic bool          ready_cache;
    _Atomic bool          closed;

    iowait_slab_t*        slab;
    uint32_t              slot_index;
};

struct iowait_slab_s {
    mtx_t              lock;
    _Atomic(iowait_t*) pages[IOWAIT_PAGES_MAX];
    _Atomic uint32_t   npages;
    uint32_t           free_slot;
};

static inline iowait_t* _iowait_slab_at(
    iowait_slab_t* slab, uint32_t index) {
    uint32_t zi     = index - 1;
    uint32_t page   = zi >> IOWAIT_PAGE_SHIFT;
    uint32_t offset = zi & (IOWAIT_PAGE_SIZE - 1);
    /**
     * Acquire pairs with the release store in _iowait_slab_alloc so a
     * lockless reader (iowait_on_event) sees the fully initialized page.
     */
    iowait_t* base =
        atomic_load_explicit(&slab->pages[page], memory_order_acquire);
    return &base[offset];
}

static inline uint32_t _iowait_slot_next(iowait_t* slot) {
    uint32_t v;
    memcpy(&v, slot, sizeof(v));
    return v;
}

static inline void _iowait_slot_set_next(iowait_t* slot, uint32_t next) {
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

    if (slab->free_slot != IOWAIT_FREE_END) {
        uint32_t  idx = slab->free_slot;
        iowait_t* w   = _iowait_slab_at(slab, idx);
        slab->free_slot = _iowait_slot_next(w);
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

    /* npages only changes under the lock; relaxed read is fine here. */
    uint32_t npages = atomic_load_explicit(&slab->npages, memory_order_relaxed);

    /**
     * +1 so that index 0 is never used: encode(0, 0) == NULL, which
     * the scheduler reserves for the wakeup-fd sentinel.
     */
    uint32_t base = npages * IOWAIT_PAGE_SIZE + 1;

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

    /**
     * Publish the fully-initialized page with release stores; lockless
     * readers acquire-load these in _iowait_slab_at. Store the slot before
     * bumping the count so a reader that observes npages also sees the page.
     */
    atomic_store_explicit(&slab->pages[npages], page, memory_order_release);
    atomic_store_explicit(&slab->npages, npages + 1, memory_order_release);

    for (uint32_t i = IOWAIT_PAGE_SIZE - 1; i >= 1; i--) {
        _iowait_slot_set_next(&page[i], slab->free_slot);
        slab->free_slot = base + i;
    }

    *out_index = base;
    mtx_unlock(&slab->lock);
    return &page[0];
}

static void _iowait_slab_free(iowait_slab_t* slab, uint32_t index) {
    iowait_t* w = _iowait_slab_at(slab, index);
    mtx_lock(&slab->lock);
    _iowait_slot_set_next(w, slab->free_slot);
    slab->free_slot = index;
    mtx_unlock(&slab->lock);
}

/**
 * Last ref dropped: bump gen, return slot to slab freelist.
 * Timers are kept alive for reuse by the next occupant.
 */
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

/* Reject stale CQE: acquire ref only if alive and gen matches. */
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

static inline bool _iowait_state_ready(uint32_t state) {
    return (state & IOWAIT_READY_BIT) != 0;
}

static inline uint32_t _iowait_state_tick(uint32_t state) {
    return state >> 1;
}

static inline uint32_t _iowait_state_pack(uint32_t tick, bool ready) {
    return (tick << 1) | (ready ? IOWAIT_READY_BIT : 0);
}

static iowait_ready_event_t _iowait_ready_event(_iowait_dir_t* d) {
    uint32_t state = atomic_load_explicit(&d->state, memory_order_acquire);
    return (iowait_ready_event_t){
        .tick = _iowait_state_tick(state),
        .ready = _iowait_state_ready(state),
    };
}

static void _iowait_set_ready(_iowait_dir_t* d) {
    uint32_t state = atomic_load_explicit(&d->state, memory_order_acquire);
    for (;;) {
        uint32_t tick = _iowait_state_tick(state) + 1;
        uint32_t next = _iowait_state_pack(tick, true);
        if (atomic_compare_exchange_weak_explicit(
                &d->state, &state, next,
                memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
}

static void _iowait_set_ready_if_needed(_iowait_dir_t* d, bool force_tick) {
    uint32_t state = atomic_load_explicit(&d->state, memory_order_acquire);
    if (!force_tick && _iowait_state_ready(state)) {
        return;
    }
    for (;;) {
        uint32_t tick = _iowait_state_tick(state) + 1;
        uint32_t next = _iowait_state_pack(tick, true);
        if (atomic_compare_exchange_weak_explicit(
                &d->state, &state, next,
                memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
        if (!force_tick && _iowait_state_ready(state)) {
            return;
        }
    }
}

static bool _iowait_clear_ready(
    _iowait_dir_t* d, iowait_ready_event_t ev) {
    uint32_t state = atomic_load_explicit(&d->state, memory_order_acquire);
    for (;;) {
        if (_iowait_state_tick(state) != ev.tick
            || !_iowait_state_ready(state)) {
            return false;
        }
        uint32_t next = _iowait_state_pack(ev.tick, false);
        if (atomic_compare_exchange_weak_explicit(
                &d->state, &state, next,
                memory_order_acq_rel, memory_order_acquire)) {
            return true;
        }
    }
}

static inline bool _iowait_use_ready_cache(iowait_t* w) {
    return PLATFORM_POLLER_TRIGGER_MODE == PLATFORM_POLLER_TRIGGER_ET
        && atomic_load_explicit(&w->ready_cache, memory_order_acquire);
}

/* Register parked directions; readiness-cache handles stay registered RD|WR. */
static void _iowait_arm(iowait_t* w) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return;
    }

    if (_iowait_use_ready_cache(w)) {
        if ((platform_poller_op_t)atomic_load_explicit(
                &w->interest, memory_order_acquire)
            == PLATFORM_POLLER_RW_OP) {
            return;
        }
    } else if (PLATFORM_POLLER_TRIGGER_MODE == PLATFORM_POLLER_TRIGGER_ET) {
        platform_poller_op_t parked = PLATFORM_POLLER_NO_OP;
        if (atomic_load_explicit(&w->rd.park, memory_order_acquire)) {
            parked |= PLATFORM_POLLER_RD_OP;
        }
        if (atomic_load_explicit(&w->wr.park, memory_order_acquire)) {
            parked |= PLATFORM_POLLER_WR_OP;
        }
        platform_poller_op_t current = (platform_poller_op_t)
            atomic_load_explicit(&w->interest, memory_order_acquire);
        platform_poller_op_t op =
            (current & PLATFORM_POLLER_RD_OP)
          | (parked & PLATFORM_POLLER_RD_OP)
          | (parked & PLATFORM_POLLER_WR_OP);
        if (op == current) {
            return;
        }
    }

    mtx_lock(&w->arm_lock);

    /* Re-check under the lock: iowait_close may have raced in. */
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        mtx_unlock(&w->arm_lock);
        return;
    }

    if (_iowait_use_ready_cache(w)) {
        platform_poller_op_t current = (platform_poller_op_t)
            atomic_load_explicit(&w->interest, memory_order_relaxed);
        w->sqe.op = PLATFORM_POLLER_RW_OP;
        if (current == PLATFORM_POLLER_NO_OP) {
            if (platform_poller_add(w->poller, &w->sqe) == 0) {
                atomic_store_explicit(
                    &w->registered, true, memory_order_release);
                atomic_store_explicit(
                    &w->interest, PLATFORM_POLLER_RW_OP,
                    memory_order_release);
            }
        } else if (current != PLATFORM_POLLER_RW_OP) {
            if (platform_poller_mod(w->poller, &w->sqe) == 0) {
                atomic_store_explicit(
                    &w->interest, PLATFORM_POLLER_RW_OP,
                    memory_order_release);
            }
        }
        mtx_unlock(&w->arm_lock);
        return;
    }

    platform_poller_op_t parked = PLATFORM_POLLER_NO_OP;
    if (atomic_load_explicit(&w->rd.park, memory_order_acquire)) {
        parked |= PLATFORM_POLLER_RD_OP;
    }
    if (atomic_load_explicit(&w->wr.park, memory_order_acquire)) {
        parked |= PLATFORM_POLLER_WR_OP;
    }

    platform_poller_op_t current = (platform_poller_op_t)
        atomic_load_explicit(&w->interest, memory_order_relaxed);

    platform_poller_op_t op = parked;
    if (PLATFORM_POLLER_TRIGGER_MODE == PLATFORM_POLLER_TRIGGER_ET) {
        /*
         * Read readiness is edge-triggered and does not churn while idle, so
         * keep it registered after the first read park. Write readiness is
         * usually true for TCP sockets and can flood the poller when no writer
         * is parked, so only keep WR while there is an active write waiter.
         */
        op = (current & PLATFORM_POLLER_RD_OP)
           | (parked & PLATFORM_POLLER_RD_OP)
           | (parked & PLATFORM_POLLER_WR_OP);
    }

    if (op == PLATFORM_POLLER_NO_OP) {
        if (current != PLATFORM_POLLER_NO_OP) {
            platform_poller_del(w->poller, &w->sqe);
            atomic_store_explicit(
                &w->registered, false, memory_order_release);
            atomic_store_explicit(
                &w->interest, PLATFORM_POLLER_NO_OP, memory_order_release);
        }
        w->sqe.op = PLATFORM_POLLER_NO_OP;
        mtx_unlock(&w->arm_lock);
        return;
    }

    if (current == PLATFORM_POLLER_NO_OP) {
        w->sqe.op = op;
        if (platform_poller_add(w->poller, &w->sqe) == 0) {
            atomic_store_explicit(
                &w->registered, true, memory_order_release);
            atomic_store_explicit(
                &w->interest, op, memory_order_release);
        }
    } else if (w->sqe.op != op) {
        w->sqe.op = op;
        if (platform_poller_mod(w->poller, &w->sqe) == 0) {
            atomic_store_explicit(&w->interest, op, memory_order_release);
        }
    }

    mtx_unlock(&w->arm_lock);
}

static void _iowait_wake(mco_coro* co) {
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

static void _iowait_wake_batch(
    scheduler_t* sched, runnable_batch_t* batch, mco_coro* co) {
    if (!co) {
        return;
    }
    if (batch->n == batch->cap) {
        scheduler_schedule_batch(sched, batch->coros, batch->n);
        batch->n = 0;
    }
    batch->coros[batch->n++] = co;
}

static void _iowait_timeout_cb(scheduler_timer_t* timer, void* ud) {
    (void)timer;
    _iowait_dir_t* d = (_iowait_dir_t*)ud;
    iowait_t*      w = d->w;
    _iowait_wake(atomic_exchange(&d->park, NULL));
    _iowait_unref(w);
}

/* Re-check close/deadline after publish to catch races with concurrent wakers. */
static bool _iowait_park_cb(mco_coro* co, void* arg) {
    _iowait_dir_t* d = (_iowait_dir_t*)arg;
    iowait_t*      w = d->w;

    /**
     * Publish the park record, then re-check close/deadline. This is a
     * Dekker handshake against a concurrent waker (iowait_close /
     * deadline timer), which stores its cause then exchanges park->NULL.
     * It only works if the publish (store) and the re-check (load) are
     * ordered StoreLoad, which release/acquire does NOT provide -- both
     * sides must participate in the single total order, so the publish
     * and the re-check loads below are seq_cst (matching the seq_cst
     * `closed` store in iowait_close and the seq_cst park exchanges done
     * by every waker). Without this a waker could miss the not-yet-
     * visible park while the parker misses the not-yet-visible cause,
     * stranding the coroutine forever.
     */
    mco_coro* prev = atomic_exchange_explicit(
        &d->park, co, memory_order_seq_cst);
    if (prev != NULL) {
        xylem_loge(
            "<iowait> double park dir=%s w=%p prev=%p new=%p",
            (d == &w->rd) ? "rd" : "wr",
            (void*)w, (void*)prev, (void*)co);
        abort();
    }

    _iowait_arm(w);

    /* Re-check after publish: readiness, close or deadline may have raced in. */
    if (_iowait_use_ready_cache(w) && _iowait_ready_event(d).ready) {
        _iowait_wake(atomic_exchange(&d->park, NULL));
    } else if (atomic_load_explicit(&w->closed, memory_order_seq_cst)) {
        _iowait_wake(atomic_exchange(&d->park, NULL));
    } else {
        uint64_t dl = atomic_load_explicit(
            &d->deadline, memory_order_seq_cst);
        if (dl > 0 && xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= dl) {
            _iowait_wake(atomic_exchange(&d->park, NULL));
        }
    }
    return true;
}

/* Each timer arm owns one iowait ref, released in the timeout cb or on rearm cancel. */
static void _iowait_set_deadline(_iowait_dir_t* d, uint64_t deadline_ms) {
    atomic_store_explicit(&d->deadline, deadline_ms, memory_order_release);

    /**
     * Cancel any timer arm still in flight; if we actually caught it
     * before it fired, return the reference that arm owned.
     */
    if (d->timer && scheduler_timer_stop(d->timer)) {
        _iowait_unref(d->w);
    }

    if (deadline_ms == 0) {
        return;
    }

    if (!d->timer) {
        d->timer = scheduler_timer_create(runtime_get_scheduler());
        if (!d->timer) {
            return;
        }
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    uint64_t in  = (deadline_ms > now) ? (deadline_ms - now) : 0;

    _iowait_ref(d->w);
    scheduler_timer_start(d->timer, _iowait_timeout_cb, d, in, 0);
}

static iowait_result_t _iowait_wait(iowait_t* w, _iowait_dir_t* d) {
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return IOWAIT_CLOSED;
    }
    if (_iowait_use_ready_cache(w) && _iowait_ready_event(d).ready) {
        return IOWAIT_READY;
    }

    scheduler_park(runtime_get_scheduler(), _iowait_park_cb, d);

    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        return IOWAIT_CLOSED;
    }
    if (_iowait_use_ready_cache(w) && _iowait_ready_event(d).ready) {
        return IOWAIT_READY;
    }
    uint64_t dl = atomic_load_explicit(&d->deadline, memory_order_acquire);
    if (dl > 0 && xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= dl) {
        return IOWAIT_TIMEOUT;
    }
    return IOWAIT_READY;
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
    slab->free_slot = IOWAIT_FREE_END;
    return slab;
}

void iowait_slab_destroy(iowait_slab_t* slab) {
    if (!slab) {
        return;
    }
    uint32_t npages = atomic_load_explicit(&slab->npages, memory_order_relaxed);
    for (uint32_t p = 0; p < npages; p++) {
        iowait_t* page =
            atomic_load_explicit(&slab->pages[p], memory_order_relaxed);
        for (uint32_t i = 0; i < IOWAIT_PAGE_SIZE; i++) {
            if (page[i].rd.timer) {
                scheduler_timer_destroy(page[i].rd.timer);
            }
            if (page[i].wr.timer) {
                scheduler_timer_destroy(page[i].wr.timer);
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
    atomic_store_explicit(&w->rd.state, 0, memory_order_relaxed);
    atomic_store_explicit(&w->wr.state, 0, memory_order_relaxed);
    atomic_store_explicit(&w->rd.active, false, memory_order_relaxed);
    atomic_store_explicit(&w->wr.active, false, memory_order_relaxed);
    atomic_store_explicit(&w->rd.deadline, 0, memory_order_relaxed);
    atomic_store_explicit(&w->wr.deadline, 0, memory_order_relaxed);
    atomic_store_explicit(&w->closed, false, memory_order_relaxed);
    atomic_store_explicit(&w->registered, false, memory_order_relaxed);
    atomic_store_explicit(&w->interest, PLATFORM_POLLER_NO_OP, memory_order_relaxed);
    atomic_store_explicit(&w->ready_cache, false, memory_order_relaxed);

    w->slab = slab;

    w->poller = runtime_get_poller();
    w->fd     = fd;

    w->sqe.fd = (platform_poller_fd_t)fd;
    w->sqe.op = PLATFORM_POLLER_NO_OP;
    uint16_t gen = atomic_load_explicit(&w->gen, memory_order_relaxed);
    w->sqe.ud = _iowait_ud_encode(index, gen);

    w->rd.w = w;
    w->wr.w = w;

    _iowait_ref(w);
    return w;
}

void iowait_enable_readiness_cache(iowait_t* w) {
    if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
        return;
    }
    atomic_store_explicit(&w->ready_cache, true, memory_order_release);
    _iowait_arm(w);
}

iowait_ready_event_t iowait_read_ready_event(iowait_t* w) {
    return _iowait_ready_event(&w->rd);
}

iowait_ready_event_t iowait_write_ready_event(iowait_t* w) {
    return _iowait_ready_event(&w->wr);
}

void iowait_clear_read_ready(iowait_t* w, iowait_ready_event_t ev) {
    (void)_iowait_clear_ready(&w->rd, ev);
}

void iowait_clear_write_ready(iowait_t* w, iowait_ready_event_t ev) {
    (void)_iowait_clear_ready(&w->wr, ev);
}

void iowait_set_write_active(iowait_t* w, bool active) {
    atomic_store_explicit(&w->wr.active, active, memory_order_release);
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
            memory_order_seq_cst,
            memory_order_acquire)) {
        return;
    }

    /* Drop poller subscription now so the caller can safely close the fd. */
    mtx_lock(&w->arm_lock);
    if (atomic_load_explicit(&w->registered, memory_order_relaxed)) {
        platform_poller_del(w->poller, &w->sqe);
        atomic_store_explicit(&w->registered, false, memory_order_relaxed);
        atomic_store_explicit(
            &w->interest, PLATFORM_POLLER_NO_OP, memory_order_release);
    }
    mtx_unlock(&w->arm_lock);

    _iowait_wake(atomic_exchange(&w->rd.park, NULL));
    _iowait_wake(atomic_exchange(&w->wr.park, NULL));
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
    iowait_t*      w     = _iowait_tryref(_iowait_slab_at(slab, index), tag);
    if (!w) {
        return;
    }

    bool woke = false;
    if (revents & PLATFORM_POLLER_RD_OP) {
        if (_iowait_use_ready_cache(w)) {
            _iowait_set_ready(&w->rd);
            mco_coro* co = atomic_load_explicit(
                &w->rd.park, memory_order_acquire);
            if (co) {
                co = atomic_exchange(&w->rd.park, NULL);
            }
            woke = woke || (co != NULL);
            _iowait_wake_batch(sched, batch, co);
        } else {
            mco_coro* co = atomic_exchange(&w->rd.park, NULL);
            woke = woke || (co != NULL);
            _iowait_wake_batch(sched, batch, co);
        }
    }
    if (revents & PLATFORM_POLLER_WR_OP) {
        if (_iowait_use_ready_cache(w)) {
            mco_coro* co = atomic_load_explicit(
                &w->wr.park, memory_order_acquire);
            bool active = atomic_load_explicit(
                &w->wr.active, memory_order_acquire);
            if (!co && !active
                && _iowait_ready_event(&w->wr).ready) {
                /* Cached EPOLLOUT with no waiter: no scheduling or RMW. */
            } else {
                if (co) {
                    co = atomic_exchange(&w->wr.park, NULL);
                }
                _iowait_set_ready_if_needed(&w->wr, co != NULL || active);
                woke = woke || (co != NULL);
                _iowait_wake_batch(sched, batch, co);
            }
        } else {
            mco_coro* co = atomic_exchange(&w->wr.park, NULL);
            woke = woke || (co != NULL);
            _iowait_wake_batch(sched, batch, co);
        }
    }

    /*
     * LT+oneshot must re-arm immediately. ET can defer cleanup after a real
     * wake: the resumed coroutine will re-arm when it parks again, moving the
     * WR-off epoll_ctl out of the poll batch. If no waiter was woken, clean up
     * stale interest now to avoid empty writable-event churn.
     */
    if (!_iowait_use_ready_cache(w)
        && (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET
            || !woke)) {
        _iowait_arm(w);
    }

    _iowait_unref(w);
}
