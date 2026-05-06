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

#include "c11-threads.h"
#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct _iowait_park_s _iowait_park_t;
typedef struct _iowait_dir_s  _iowait_dir_t;

/**
 * Per-direction state. Read and write are fully symmetric; bundling
 * them into one struct eliminates the rd/wr code duplication and
 * makes the two iowait slots share a single set of helpers.
 *
 *   w         - back-pointer to the owning iowait. Needed because the
 *               timer callback receives this struct as ud and must
 *               drop a ref on the iowait itself.
 *   park      - atomic slot holding the currently parked coroutine,
 *               or NULL when no one is parked. All wakers race on
 *               this slot via _iowait_claim (see below).
 *   timer     - deadline timer, allocated lazily on the first
 *               non-zero deadline and reused afterwards. NULL means
 *               "no deadline has ever been set on this direction".
 *               Accessed only from the deadline-setter path and from
 *               _iowait_do_free; not safe against concurrent
 *               deadline sets on the same direction.
 *   deadline  - absolute ms deadline or 0. Atomically published so
 *               a parking waiter can observe an updated value before
 *               committing to sleep.
 */
struct _iowait_dir_s {
    iowait_t*                w;
    _Atomic(_iowait_park_t*) park;
    sched_timer_t*           timer;
    _Atomic uint64_t         deadline;
};

/**
 * Per-park state kept on the caller's stack. The iowait publishes a
 * pointer to this via an atomic slot (dir->park). Wakers race to
 * atomically swap the slot back to NULL: whichever thread succeeds
 * sets `result` and reschedules park->co. The park record is only
 * valid for the duration of the scheduler_park call, which is why
 * every waker must go through _iowait_claim before touching it.
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
 *   poller      - shared scheduler poller this fd is registered on.
 *   sqe/fd      - poller submission entry and raw fd. `sqe.ud` always
 *                 points back to this handle so iowait_on_event can
 *                 recover context.
 *   rd / wr     - per-direction state; see _iowait_dir_s above.
 *   arm_lock    - serialises every entry into _iowait_arm. Two arm
 *                 sources race on the same handle: a parking worker
 *                 (via _iowait_park_fn) and the poller thread
 *                 (via iowait_on_event). Without serialisation they
 *                 would both compute an op from a stale view of
 *                 rd.park/wr.park, write the shared sqe, and race
 *                 into platform_poller_mod -- the last MOD wins and
 *                 can drop a direction that the other thread just
 *                 published. The lock covers the full "observe park
 *                 slots -> publish sqe -> submit to poller" sequence
 *                 on LT+oneshot pollers; on ET the fast path returns
 *                 before taking the lock once the fd is registered.
 *   refcnt      - reference count keeping the handle alive while
 *                 waiters are parked or the poller still holds a
 *                 callback pointer. See _iowait_ref / _iowait_unref.
 *   registered  - whether the fd is currently in the poller. Under
 *                 ET we set this once on the first arm; under LT+
 *                 oneshot we keep flipping between add and mod.
 *                 Protected by arm_lock on the LT+oneshot path; on
 *                 ET, the fast-path read outside the lock is safe
 *                 because `registered` only transitions false->true
 *                 exactly once and a spurious "false" read just
 *                 falls through to the locked slow path.
 *   closed      - set by iowait_close; observed on park/wait entry
 *                 and on event dispatch to short-circuit operations.
 */
struct iowait_s {
    platform_poller_sq_t* poller;
    platform_poller_sqe_t sqe;
    platform_sock_t       fd;

    _iowait_dir_t         rd;
    _iowait_dir_t         wr;

    mtx_t                 arm_lock;

    _Atomic int32_t       refcnt;
    bool                  registered;
    _Atomic bool          closed;
};

/**
 * Drop the handle's creation-time reference and, if this was the last
 * ref, actually release resources. The refcnt's acq_rel on the final
 * decrement synchronises with every other release, so by the time we
 * get here no other thread can still be touching the handle; simple
 * non-atomic reads of `registered` and `timer` are safe.
 *
 * By the time refcnt reaches zero, no arm-reference can still be
 * outstanding (the arm-ref itself would keep refcnt non-zero), so
 * each timer is guaranteed to be either inactive or already-fired,
 * and sched_timer_destroy here can just drop the creator ref.
 */
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
    mtx_destroy(&w->arm_lock);
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
 *
 * Called from the park path (parking worker) and from iowait_on_event
 * (poller worker). These can be different threads, and on LT+oneshot
 * both paths must read rd.park/wr.park, write the shared sqe, and
 * submit it to the kernel. Doing that lock-free is not enough:
 * plain stores to `sqe.op`/`registered` race as data races, and
 * even if those were atomic the final EPOLL_CTL_MOD reflects the
 * later syscall's op (computed from a stale park snapshot), so a
 * direction published between snapshot and submit can silently be
 * dropped from the kernel subscription.
 *
 * To avoid that, the entire "observe park slots -> publish sqe ->
 * submit" sequence is serialised per iowait by `arm_lock`. Cross-fd
 * arms remain fully parallel -- the lock is per-handle, not global.
 *
 * On ET the registered fast path returns before taking the lock:
 * once registered transitions true, nothing ever flips it back, so
 * the unlocked read is benign.
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
    iowait_t*      w = d->w;
    _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_TIMEOUT));
    /**
     * Drop the arm-reference taken in _iowait_set_deadline. Must be
     * the last thing we do here: once this ref is released, the
     * iowait (and therefore `d` itself) may be freed by a racing
     * iowait_destroy, so `d` is not safe to touch below this point.
     */
    _iowait_unref(w);
}

/**
 * Park body executed by scheduler_park after the coroutine has
 * saved its context. Publishes the park record, (re-)arms the fd,
 * and then guards against two races that can swallow the wakeup:
 *
 *   - iowait_close() running between our park-slot store and the
 *     moment we yield. The closer's _iowait_claim would have seen
 *     a NULL slot and skipped us, leaving us sleeping forever.
 *   - The deadline timer firing before we published the park, which
 *     similarly leaves _iowait_timeout_cb with nothing to claim.
 *
 * In both cases we re-observe the condition after the publish and
 * self-wake by claiming our own park slot.
 */

static bool _iowait_park_fn(mco_coro* co, void* arg) {
    _iowait_park_t* p = (_iowait_park_t*)arg;
    iowait_t*       w = p->w;
    _iowait_dir_t*  d = p->dir;

    p->co     = co;
    p->result = IOWAIT_READY;

    atomic_store_explicit(&d->park, p, memory_order_release);

    _iowait_arm(w);

    /* Close-race check: see _iowait_park_fn docstring. */
    if (atomic_load_explicit(&w->closed, memory_order_acquire)) {
        _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_CLOSED));
        return true;
    }

    /* Deadline-race check: see _iowait_park_fn docstring. */
    uint64_t deadline = atomic_load_explicit(
        &d->deadline, memory_order_acquire);
    if (_iowait_deadline_passed(deadline)) {
        _iowait_wake_park(_iowait_claim(&d->park, IOWAIT_TIMEOUT));
    }
    return true;
}

/**
 * Apply a new deadline to a direction.
 *
 * The deadline word is published first so a waiter that is about to
 * park (or has just published its park record) can observe it via the
 * acquire load in _iowait_park_fn / _iowait_wait.
 *
 * The timer is allocated lazily on the first non-zero deadline: most
 * iowaits (for example short-lived accept fds) never set a deadline
 * and pay no timer cost. The flip side is that the timer pointer is
 * a plain field and must not be mutated concurrently on the same
 * direction; the public API documents this constraint. See the
 * header for the exact threading rules.
 *
 * Refcount protocol vs the deadline timer: each successful (re-)arm
 * owns one reference on the iowait; it is released either by the
 * callback when the timer fires, or here, when a subsequent call
 * cancels a still-pending fire via sched_timer_stop(). The scheduler
 * already keeps the timer object itself alive across an in-flight
 * fire, so combined with this ref the callback always sees a live
 * iowait even if iowait_destroy races in.
 *
 * Starting a timer that is already armed (rearm) implicitly cancels
 * the previous arm; sched_timer_start's return is not inspected,
 * instead the same effect is achieved by stop-then-start here, so the
 * cancelled arm's ref is returned before a new one is taken.
 *
 * Known semantic corner case: passing 0 may still result in one late
 * IOWAIT_TIMEOUT delivery if the timer's callback was already
 * dispatched before sched_timer_stop acquired the timer lock. The
 * refcount protocol above guarantees this is memory-safe; it does
 * not eliminate the possibility of an extra wake.
 */
static void _iowait_set_deadline(_iowait_dir_t* d, uint64_t deadline_ms) {
    atomic_store_explicit(&d->deadline, deadline_ms, memory_order_release);

    if (deadline_ms == 0) {
        if (d->timer && sched_timer_stop(d->timer)) {
            _iowait_unref(d->w);
        }
        return;
    }

    if (!d->timer) {
        d->timer = sched_timer_create(runtime_get_scheduler());
        if (!d->timer) {
            return;
        }
    } else if (sched_timer_stop(d->timer)) {
        /* Return the ref owned by the previous arm we just cancelled. */
        _iowait_unref(d->w);
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    uint64_t in  = (deadline_ms > now) ? (deadline_ms - now) : 0;

    _iowait_ref(d->w);
    sched_timer_start(d->timer, _iowait_timeout_cb, d, in, 0);
}

/**
 * Body of iowait_read/iowait_write.
 *
 * Fast paths: already-closed handle and already-past deadline both
 * return immediately without touching the scheduler. On the slow
 * path we take a ref to keep the handle alive across the park (a
 * parallel iowait_destroy would otherwise free us out from under
 * the waiter) and then yield via scheduler_park. `park.result` is
 * stamped by whichever waker reaches _iowait_claim first.
 */
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

    if (mtx_init(&w->arm_lock, mtx_plain) != thrd_success) {
        free(w);
        return NULL;
    }

    w->poller = runtime_get_poller();
    w->fd     = fd;

    w->sqe.fd = (platform_poller_fd_t)fd;
    w->sqe.ud = w;
    w->sqe.op = PLATFORM_POLLER_NO_OP;

    w->rd.w = w;
    w->wr.w = w;

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
     * close-race check in _iowait_park_fn after publishing. The CAS
     * above orders our `closed=true` store before the park's acquire
     * load of `closed`, so the waiter is guaranteed to see the new
     * value on the self-rescue path.
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
