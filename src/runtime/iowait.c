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
#include "xylem/xylem-threads.h"

#include "runtime.h"
#include "scheduler.h"

#include "minicoro/minicoro.h"

#include <inttypes.h>
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
#define IOWAIT_PAGES_MAX  (sizeof(void*) <= 4 ? 255 : 4096)

_Static_assert(
    (uint64_t)IOWAIT_PAGES_MAX * IOWAIT_PAGE_SIZE <=
        IOWAIT_INDEX_MASK,
    "slab capacity exceeds addressable index range");

typedef struct _iowait_dir_s _iowait_dir_t;

typedef enum _iowait_waiter_state_e {
    IOWAIT_WAITER_NONE  = 0,
    IOWAIT_WAITER_WAIT  = 1,
    IOWAIT_WAITER_READY = 2,
} _iowait_waiter_state_t;

typedef enum _iowait_poll_info_e {
    IOWAIT_INFO_CLOSED     = 1u << 0,
    IOWAIT_INFO_RD_TIMEOUT = 1u << 1,
    IOWAIT_INFO_WR_TIMEOUT = 1u << 2,
    IOWAIT_INFO_RD_ERROR   = 1u << 3,
    IOWAIT_INFO_WR_ERROR   = 1u << 4,
} _iowait_poll_info_t;

_Static_assert(_Alignof(mco_coro) > IOWAIT_WAITER_READY,
               "iowait coroutine pointers must not alias sentinel states");

struct _iowait_dir_s {
    iowait_t*          w;
    _Atomic uintptr_t  waiter;  /* NONE | WAIT | READY | coroutine ptr */
    scheduler_timer_t* timer;
    mtx_t              deadline_lock;
    _Atomic uint64_t   deadline;
};

struct iowait_s {
    platform_poller_sq_t* poller;
    platform_poller_sqe_t sqe;
    platform_sock_t       fd;

    _iowait_dir_t         rd;
    _iowait_dir_t         wr;

    mtx_t                 poll_lock; /* Close/readiness/LT re-arm ordering. */

    _Atomic int32_t       refcnt;
    /**
     * Guards stale poller events after slot reuse. A 16-bit wrap is only
     * a theoretical ABA risk: it would require the same slot to be reused
     * 65536 times while an older CQE for that slot is still pending.
     */
    _Atomic uint16_t      gen;
    _Atomic int           interest;    /* PLATFORM_POLLER_NO_OP when not subscribed */
    _Atomic uint32_t      poll_info;

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
    iowait_slab_t* slab,
    uint32_t       index) {
    uint32_t zi     = index - 1;
    uint32_t page   = zi >> IOWAIT_PAGE_SHIFT;
    uint32_t offset = zi & (IOWAIT_PAGE_SIZE - 1);
    /**
     * Lockless readers must observe the page after _iowait_slab_alloc
     * publishes the fully initialized slots.
     */
    iowait_t* base = atomic_load(&slab->pages[page]);
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

static inline uint16_t _iowait_ud_gen(void* ud) {
    return (uint16_t)((uintptr_t)ud >> IOWAIT_GEN_SHIFT);
}

static iowait_t* _iowait_slab_alloc(
    iowait_slab_t* slab,
    uint32_t*      out_index) {
    mtx_lock(&slab->lock);

    if (slab->free_slot != IOWAIT_FREE_END) {
        uint32_t  idx = slab->free_slot;
        iowait_t* w   = _iowait_slab_at(slab, idx);
        slab->free_slot = _iowait_slot_next(w);
        *out_index = idx;
        mtx_unlock(&slab->lock);
        return w;
    }

    if (atomic_load(&slab->npages) >= IOWAIT_PAGES_MAX) {
        mtx_unlock(&slab->lock);
        return NULL;
    }

    iowait_t* page =
        (iowait_t*)calloc(IOWAIT_PAGE_SIZE, sizeof(iowait_t));
    if (!page) {
        mtx_unlock(&slab->lock);
        return NULL;
    }

    uint32_t npages = atomic_load(&slab->npages);

    /**
     * +1 so that index 0 is never used: encode(0, 0) == NULL, which
     * the scheduler reserves for the wakeup-fd sentinel.
     */
    uint32_t base = npages * IOWAIT_PAGE_SIZE + 1;

    for (uint32_t i = 0; i < IOWAIT_PAGE_SIZE; i++) {
        if (mtx_init(&page[i].poll_lock, mtx_plain) != thrd_success) {
            for (uint32_t j = 0; j < i; j++) {
                mtx_destroy(&page[j].poll_lock);
                mtx_destroy(&page[j].rd.deadline_lock);
                mtx_destroy(&page[j].wr.deadline_lock);
            }
            free(page);
            mtx_unlock(&slab->lock);
            return NULL;
        }
        if (mtx_init(&page[i].rd.deadline_lock, mtx_plain)
            != thrd_success) {
            for (uint32_t j = 0; j < i; j++) {
                mtx_destroy(&page[j].poll_lock);
                mtx_destroy(&page[j].rd.deadline_lock);
                mtx_destroy(&page[j].wr.deadline_lock);
            }
            mtx_destroy(&page[i].poll_lock);
            free(page);
            mtx_unlock(&slab->lock);
            return NULL;
        }
        if (mtx_init(&page[i].wr.deadline_lock, mtx_plain)
            != thrd_success) {
            for (uint32_t j = 0; j < i; j++) {
                mtx_destroy(&page[j].poll_lock);
                mtx_destroy(&page[j].rd.deadline_lock);
                mtx_destroy(&page[j].wr.deadline_lock);
            }
            mtx_destroy(&page[i].rd.deadline_lock);
            mtx_destroy(&page[i].poll_lock);
            free(page);
            mtx_unlock(&slab->lock);
            return NULL;
        }
        page[i].slot_index = base + i;
    }

    /**
     * Publish the page before bumping the count so a reader that observes
     * npages also sees the initialized page.
     */
    atomic_store(&slab->pages[npages], page);
    atomic_store(&slab->npages, npages + 1);

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

static void _iowait_retire(iowait_t* w) {
    atomic_fetch_add(&w->gen, 1);
    _iowait_slab_free(w->slab, w->slot_index);
}

static void _iowait_ref(iowait_t* w) {
    atomic_fetch_add(&w->refcnt, 1);
}

static void _iowait_unref(iowait_t* w) {
    if (atomic_fetch_sub(&w->refcnt, 1) == 1) {
        _iowait_retire(w);
    }
}

/* Reject stale CQE: acquire ref only if alive and gen matches. */
static iowait_t* _iowait_try_ref(iowait_t* w, uint16_t expected_gen) {
    int32_t cur = atomic_load(&w->refcnt);
    for (;;) {
        if (cur <= 0) {
            return NULL;
        }
        if (atomic_compare_exchange_weak(&w->refcnt, &cur, cur + 1)) {
            break;
        }
    }

    uint16_t actual = atomic_load(&w->gen);
    if (actual != expected_gen) {
        _iowait_unref(w);
        return NULL;
    }
    return w;
}

static mco_coro* _iowait_transition_waiter(
    _iowait_dir_t*         d,
    _iowait_waiter_state_t next_state) {
    uintptr_t raw = atomic_load(&d->waiter);
    for (;;) {
        if (next_state == IOWAIT_WAITER_READY) {
            if (raw == IOWAIT_WAITER_READY) {
                return NULL;
            }
        } else {
            if (raw == IOWAIT_WAITER_NONE
                || raw == IOWAIT_WAITER_READY) {
                return NULL;
            }
        }

        if (atomic_compare_exchange_weak(
                &d->waiter,
                &raw,
                next_state)) {
            if (raw > IOWAIT_WAITER_READY) {
                return (mco_coro*)raw;
            }
            return NULL;
        }
    }
}

static bool _iowait_take_ready(_iowait_dir_t* d) {
    uintptr_t expected = IOWAIT_WAITER_READY;
    return atomic_compare_exchange_strong(
        &d->waiter, &expected, IOWAIT_WAITER_NONE);
}

static _iowait_poll_info_t _iowait_timeout_mask(const _iowait_dir_t* d) {
    return d == &d->w->rd ? IOWAIT_INFO_RD_TIMEOUT
                          : IOWAIT_INFO_WR_TIMEOUT;
}

static _iowait_poll_info_t _iowait_error_mask(const _iowait_dir_t* d) {
    return d == &d->w->rd ? IOWAIT_INFO_RD_ERROR : IOWAIT_INFO_WR_ERROR;
}

static bool _iowait_deadline_expired(_iowait_dir_t* d) {
    uint32_t mask    = _iowait_timeout_mask(d);
    bool     expired = false;

    if (atomic_load(&d->w->poll_info) & mask) {
        return true;
    }

    uint64_t deadline = atomic_load(&d->deadline);
    if (deadline == 0
        || xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) < deadline) {
        return false;
    }

    mtx_lock(&d->deadline_lock);
    deadline = atomic_load(&d->deadline);
    expired = deadline > 0
        && xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline;
    if (expired) {
        atomic_fetch_or(&d->w->poll_info, mask);
    }
    mtx_unlock(&d->deadline_lock);
    return expired;
}

static void _iowait_timeout_cb(scheduler_timer_t* timer, void* ud) {
    (void)timer;
    _iowait_dir_t* d = (_iowait_dir_t*)ud;
    iowait_t*      w = d->w;
    mco_coro*      co = NULL;
    uint64_t       deadline;

    mtx_lock(&d->deadline_lock);
    deadline = atomic_load(&d->deadline);
    if (deadline > 0
        && xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline) {
        atomic_fetch_or(&w->poll_info, _iowait_timeout_mask(d));
        co = _iowait_transition_waiter(d, IOWAIT_WAITER_NONE);
    }
    mtx_unlock(&d->deadline_lock);

    if (co) {
        scheduler_coro_ready(runtime_get_scheduler(), co);
    }
    _iowait_unref(w);
}

static bool _iowait_wait_commit_cb(mco_coro* co, void* arg) {
    _iowait_dir_t* d = (_iowait_dir_t*)arg;
    uintptr_t expected = IOWAIT_WAITER_WAIT;
    return atomic_compare_exchange_strong(
        &d->waiter, &expected, (uintptr_t)co);
}

static void _iowait_stop_deadline(_iowait_dir_t* d) {
    mtx_lock(&d->deadline_lock);
    if (d->timer && scheduler_timer_stop(d->timer)) {
        _iowait_unref(d->w);
    }
    mtx_unlock(&d->deadline_lock);
}

/* Each timer arm owns one iowait ref, released on timeout or rearm cancel. */
static void _iowait_set_deadline(_iowait_dir_t* d, uint64_t deadline_ms) {
    mco_coro* co = NULL;

    mtx_lock(&d->deadline_lock);

    atomic_store(&d->deadline, deadline_ms);
    atomic_fetch_and(
        &d->w->poll_info,
        ~(_iowait_timeout_mask(d) | _iowait_error_mask(d)));

    if (deadline_ms == 0
        || (atomic_load(&d->w->poll_info) & IOWAIT_INFO_CLOSED)) {
        if (d->timer && scheduler_timer_stop(d->timer)) {
            _iowait_unref(d->w);
        }
        mtx_unlock(&d->deadline_lock);
        return;
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (deadline_ms <= now) {
        atomic_fetch_or(&d->w->poll_info, _iowait_timeout_mask(d));
        if (d->timer && scheduler_timer_stop(d->timer)) {
            _iowait_unref(d->w);
        }
        co = _iowait_transition_waiter(d, IOWAIT_WAITER_NONE);
        mtx_unlock(&d->deadline_lock);
        if (co) {
            scheduler_coro_ready(runtime_get_scheduler(), co);
        }
        return;
    }

    uint64_t in = deadline_ms - now;

    if (!d->timer) {
        d->timer = scheduler_timer_create(runtime_get_scheduler());
        if (!d->timer) {
            atomic_fetch_or(&d->w->poll_info, _iowait_error_mask(d));
            co = _iowait_transition_waiter(d, IOWAIT_WAITER_NONE);
            mtx_unlock(&d->deadline_lock);
            if (co) {
                scheduler_coro_ready(runtime_get_scheduler(), co);
            }
            return;
        }
        _iowait_ref(d->w);
        scheduler_timer_start(d->timer, _iowait_timeout_cb, d, in, 0);
        mtx_unlock(&d->deadline_lock);
        return;
    }

    /**
     * Non-zero deadline updates are re-arms, not stop/start pairs: stop
     * is a destroy/cancel barrier for firing timers and would discard the
     * reset scheduled below. Each new arm takes a ref; reset() returns true
     * when it cancelled an older queued arm or overwrote an older deferred
     * re-arm whose ref must be released.
     */
    _iowait_ref(d->w);
    if (scheduler_timer_reset(d->timer, in)) {
        _iowait_unref(d->w);
    }
    mtx_unlock(&d->deadline_lock);
}

static iowait_result_t _iowait_check_result(iowait_t* w, _iowait_dir_t* d) {
    if (atomic_load(&w->poll_info) & IOWAIT_INFO_CLOSED) {
        return IOWAIT_CLOSED;
    }
    if (_iowait_deadline_expired(d)) {
        return IOWAIT_TIMEOUT;
    }
    if (atomic_load(&w->poll_info) & _iowait_error_mask(d)) {
        return IOWAIT_ERROR;
    }
    return IOWAIT_READY;
}

static iowait_result_t _iowait_wait(iowait_t* w, _iowait_dir_t* d) {
    for (;;) {
        if (_iowait_take_ready(d)) {
            return IOWAIT_READY;
        }

        iowait_result_t result = _iowait_check_result(w, d);
        if (result != IOWAIT_READY) {
            return result;
        }

        uintptr_t expected = IOWAIT_WAITER_NONE;
        if (!atomic_compare_exchange_strong(
                &d->waiter, &expected, IOWAIT_WAITER_WAIT)) {
            if (expected == IOWAIT_WAITER_READY
                || expected == IOWAIT_WAITER_NONE) {
                continue;
            }

            xylem_loge(
                "<iowait> duplicate waiter dir=%s w=%p state=0x%" PRIxPTR
                " co=%p",
                (d == &w->rd) ? "rd" : "wr",
                (void*)w,
                expected,
                (void*)mco_running());
            abort();
        }

        result = _iowait_check_result(w, d);
        if (result != IOWAIT_READY) {
            _iowait_transition_waiter(d, IOWAIT_WAITER_NONE);
            continue;
        }

        scheduler_coro_park(
            runtime_get_scheduler(), _iowait_wait_commit_cb, d);
    }
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
    uint32_t npages = atomic_load(&slab->npages);
    for (uint32_t p = 0; p < npages; p++) {
        iowait_t* page = atomic_load(&slab->pages[p]);
        for (uint32_t i = 0; i < IOWAIT_PAGE_SIZE; i++) {
            if (page[i].rd.timer) {
                scheduler_timer_destroy(page[i].rd.timer);
            }
            if (page[i].wr.timer) {
                scheduler_timer_destroy(page[i].wr.timer);
            }
            mtx_destroy(&page[i].rd.deadline_lock);
            mtx_destroy(&page[i].wr.deadline_lock);
            mtx_destroy(&page[i].poll_lock);
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

    atomic_store(&w->rd.waiter, IOWAIT_WAITER_NONE);
    atomic_store(&w->wr.waiter, IOWAIT_WAITER_NONE);
    atomic_store(&w->rd.deadline, 0);
    atomic_store(&w->wr.deadline, 0);
    atomic_store(&w->poll_info, 0);
    atomic_store(&w->interest, PLATFORM_POLLER_NO_OP);
    w->slab = slab;

    w->poller = runtime_get_poller();
    w->fd     = fd;

    w->sqe.fd = (platform_poller_fd_t)fd;
    w->sqe.op = PLATFORM_POLLER_NO_OP;
    uint16_t gen = atomic_load(&w->gen);
    w->sqe.ud = _iowait_ud_encode(index, gen);

    w->rd.w = w;
    w->wr.w = w;

    _iowait_ref(w);
    w->sqe.op = PLATFORM_POLLER_RW_OP;
    if (platform_poller_add(w->poller, &w->sqe) != 0) {
        _iowait_unref(w);
        return NULL;
    }
    atomic_store(&w->interest, PLATFORM_POLLER_RW_OP);
    return w;
}

void iowait_set_rd_deadline(iowait_t* w, uint64_t deadline_ms) {
    _iowait_set_deadline(&w->rd, deadline_ms);
}

void iowait_set_wr_deadline(iowait_t* w, uint64_t deadline_ms) {
    _iowait_set_deadline(&w->wr, deadline_ms);
}

bool iowait_read_deadline_expired(iowait_t* w) {
    return _iowait_deadline_expired(&w->rd);
}

bool iowait_write_deadline_expired(iowait_t* w) {
    return _iowait_deadline_expired(&w->wr);
}

iowait_result_t iowait_read(iowait_t* w) {
    return _iowait_wait(w, &w->rd);
}

iowait_result_t iowait_write(iowait_t* w) {
    return _iowait_wait(w, &w->wr);
}

void iowait_close(iowait_t* w) {
    mco_coro* rd;
    mco_coro* wr;

    mtx_lock(&w->poll_lock);
    if (atomic_load(&w->poll_info) & IOWAIT_INFO_CLOSED) {
        mtx_unlock(&w->poll_lock);
        return;
    }
    atomic_fetch_or(&w->poll_info, IOWAIT_INFO_CLOSED);

    /* Delete under poll_lock so no event can re-arm before close returns. */
    if ((platform_poller_op_t)atomic_load(&w->interest)
        != PLATFORM_POLLER_NO_OP) {
        platform_poller_del(w->poller, &w->sqe);
        atomic_store(&w->interest, PLATFORM_POLLER_NO_OP);
    }

    rd = _iowait_transition_waiter(&w->rd, IOWAIT_WAITER_NONE);
    wr = _iowait_transition_waiter(&w->wr, IOWAIT_WAITER_NONE);
    mtx_unlock(&w->poll_lock);

    _iowait_stop_deadline(&w->rd);
    _iowait_stop_deadline(&w->wr);

    if (rd) {
        scheduler_coro_ready(runtime_get_scheduler(), rd);
    }
    if (wr) {
        scheduler_coro_ready(runtime_get_scheduler(), wr);
    }
}

void iowait_destroy(iowait_t* w) {
    if (!w) {
        return;
    }
    if (!(atomic_load(&w->poll_info) & IOWAIT_INFO_CLOSED)) {
        iowait_close(w);
    }
    _iowait_unref(w);
}

size_t iowait_process_event(
    scheduler_t* sched,
    int          revents,
    void*        ud,
    mco_coro**   runnables) {
    uint32_t       index = _iowait_ud_index(ud);
    uint16_t       gen   = _iowait_ud_gen(ud);
    iowait_slab_t* slab  = scheduler_get_iowait_slab(sched);
    iowait_t*      w     = _iowait_try_ref(_iowait_slab_at(slab, index), gen);
    if (!w) {
        return 0;
    }

    mco_coro* rd = NULL;
    mco_coro* wr = NULL;

    /**
     * Serialize readiness with close. The LT path stays in the same critical
     * section so close cannot delete the fd between readiness and re-arm.
     */
    mtx_lock(&w->poll_lock);
    if (!(atomic_load(&w->poll_info) & IOWAIT_INFO_CLOSED)) {
        if (revents & PLATFORM_POLLER_RD_OP) {
            rd = _iowait_transition_waiter(
                &w->rd,
                IOWAIT_WAITER_READY);
        }
        if (revents & PLATFORM_POLLER_WR_OP) {
            wr = _iowait_transition_waiter(
                &w->wr,
                IOWAIT_WAITER_READY);
        }
    }

    /**
     * ONESHOT auto-disabled the fd; sync interest and re-arm.
     * ET never needs this: interest stays RW_OP, kernel keeps the fd.
     */
    if (PLATFORM_POLLER_TRIGGER_MODE == PLATFORM_POLLER_TRIGGER_LT) {
        atomic_store(&w->interest, PLATFORM_POLLER_NO_OP);
        w->sqe.op = PLATFORM_POLLER_RW_OP;
        if (!(atomic_load(&w->poll_info) & IOWAIT_INFO_CLOSED)) {
            if (platform_poller_mod(w->poller, &w->sqe) != 0) {
                xylem_loge("<iowait> re-arm mod failed w=%p fd=%d",
                           (void*)w, (int)w->fd);
                abort();
            }
            atomic_store(&w->interest, PLATFORM_POLLER_RW_OP);
        }
    }
    mtx_unlock(&w->poll_lock);

    size_t count = 0;
    if (rd) {
        runnables[count++] = rd;
    }
    if (wr) {
        runnables[count++] = wr;
    }

    _iowait_unref(w);
    return count;
}
