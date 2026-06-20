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

#include "xylem/sync/xylem-sem.h"
#include "xylem/xylem-utils.h"

#include "container/list.h"
#include "platform/platform-futex.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Cross-context counting semaphore.
 *
 * The one primitive both coroutines and plain OS threads may block on,
 * so either can notify the other; the rest are coroutine-only.
 *
 * Two waiter kinds block differently, mirroring xylem_mutex:
 *
 *   - Coroutines queue FIFO on `co_waiters` (guarded by `guard`) and park on
 *     the scheduler. When no OS thread is blocked, post() hands a token to
 *     the oldest by waking it directly, never touching `count`, so the
 *     woken coroutine returns with its token in hand and none is lost --
 *     no kernel round-trip. This is the common coroutine-only fast path.
 *
 *   - OS threads do not queue: they barge on `count` (the futex word),
 *     CAS-decrementing a free token or sleeping in platform_futex_wait
 *     while it is zero. post() banks the token (count++) and wakes one via
 *     platform_futex_signal. `thr_waiters` only tells post whether a futex
 *     wake is needed, so a coroutine-only workload pays no syscall.
 *
 * Fairness across the two kinds (mirrors xylem_mutex): the direct hand-off
 * is taken ONLY while `thr_waiters == 0`. The instant a thread is blocked,
 * post() switches to the release path -- it banks the token (count++) and
 * wakes one of each to contend for it: the FIFO-oldest coroutine is woken
 * to re-contend (granted == 0) and a thread is futex-signalled. Whoever
 * CAS-takes the banked token first wins; the loser re-parks (coroutine) or
 * re-sleeps (thread). Without this, a steady stream of coroutine waiters
 * would let post() hand every token to a coroutine and starve threads
 * forever.
 *
 * A woken coroutine therefore learns how it was woken from `granted`:
 *   1 = a token was handed to it (it owns it, returns), 0 = a release-path
 *   wake to re-contend (it must _sem_try, and re-park if it loses).
 *
 * `guard` serialises the post decision (hand to a coroutine vs. bank the
 * token) against a coroutine enqueuing in its park callback: the bank
 * (count++) happens under the guard, so a coroutine racing post is either
 * seen in the list (woken) or observes the banked count in its park
 * callback (takes it) -- never stranded. Threads need no guard: a barge
 * that beats the bank just re-checks the word, and the default atomic
 * ordering between post's count++ and a thread's arm closes the
 * lost-wakeup race.
 *
 * Waiter lifetime: infinite coroutine waits keep the record on the parked
 * coroutine's stack (post snapshots it under the guard, never derefs it
 * after). A timed coroutine wait can be pulled from the queue by a timer
 * on another worker racing the resumed coroutine, so it is a refcounted
 * heap object (wait ref + timer ref) freed by whoever drops the last ref.
 */

struct xylem_sem_s {
    spin_t           guard;       /* serialises coro list + the count bank */
    _Atomic uint32_t count;       /* tokens; also the thread futex word    */
    _Atomic int32_t  thr_waiters; /* OS threads sleeping on the futex word  */
    list_t           co_waiters;     /* coroutine waiters, FIFO                */
};

static uint64_t _sem_now_ms(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
}

/**
 * Take a token if one is free: lock-free CAS-decrement of `count`, valid
 * in any context (coroutine fast path, thread barge, non-blocking try).
 */
static bool _sem_try(xylem_sem_t* s) {
    uint32_t c = atomic_load(&s->count);
    while (c > 0) {
        if (atomic_compare_exchange_weak(&s->count, &c, c - 1)) {
            return true;
        }
    }
    return false;
}

/**
 * A NULL next link means the node was never linked or was removed
 * (list_remove clears it). Read under the guard to arbitrate the
 * timeout-vs-post race.
 */
static inline bool _sem_node_linked(const list_node_t* n) {
    return n->next != NULL;
}

/**
 * Coroutine waiter record. Lives in the waiting coroutine's lock frame
 * (infinite wait) or inside the refcounted timed object below. Threads
 * never queue here -- they barge on `count`.
 */
typedef struct _sem_co_waiter_s {
    list_node_t      node;
    mco_coro*        co;
    scheduler_t*     sched;
    _Atomic uint32_t granted; /* 1 = token handed over, 0 = re-contend wake */
} _sem_co_waiter_t;

/* Timed coroutine waiter: refcounted heap object (sem is its only user). */

typedef struct _sem_co_timed_s {
    _sem_co_waiter_t base;
    xylem_sem_t*     sem;
    scheduler_timer_t*   timer;  /* NULL if creation failed */
    uint64_t         timeout_ms;
    bool             armed;   /* timer started once; guarded by sem->guard  */
    _Atomic int32_t  refcnt; /* wait ref + timer ref while armed */
    _Atomic bool     timed_out;
} _sem_co_timed_t;

static void _sem_co_ref(_sem_co_timed_t* w) {
    atomic_fetch_add(&w->refcnt, 1);
}

static void _sem_co_unref(_sem_co_timed_t* w) {
    if (atomic_fetch_sub(&w->refcnt, 1) != 1) {
        return;
    }
    /* The last ref owns the timer; destroy is safe even mid-callback. */
    if (w->timer) {
        scheduler_timer_destroy(w->timer);
    }
    free(w);
}

static void _sem_co_timeout_cb(scheduler_timer_t* timer, void* ud) {
    (void)timer;
    _sem_co_timed_t* w = (_sem_co_timed_t*)ud;
    xylem_sem_t*     s = w->sem;

    /**
     * Mark timed_out before deciding whether to schedule. Setting it
     * unconditionally (even when the node is already unlinked) closes the
     * strand window of the re-contend loop: a release-path post() may have
     * pulled the node to wake the coroutine for re-contention, and if the
     * timer fires in that gap the coroutine must still observe the timeout
     * (in its loop or its next park callback) instead of re-parking on a
     * timer that has already fired and will never fire again.
     *
     * Schedule only if the node is still linked: otherwise a post() owns
     * the wake and is resuming the coroutine, so scheduling here would be
     * a double wake.
     */
    spin_lock(&s->guard);
    bool linked = _sem_node_linked(&w->base.node);
    if (linked) {
        list_remove(&s->co_waiters, &w->base.node);
    }
    atomic_store(&w->timed_out, true);
    _sem_co_waiter_t target = w->base;
    spin_unlock(&s->guard);

    /* Last touch of the coroutine: it may drop its ref and free w. */
    if (linked) {
        atomic_store(&w->base.granted, 0);
        scheduler_schedule(target.sched, target.co);
    }
    _sem_co_unref(w);
}

typedef struct _sem_timed_ctx_s {
    xylem_sem_t*     sem;
    _sem_co_timed_t* w;
} _sem_timed_ctx_t;

static bool _sem_timed_park_cb(mco_coro* co, void* arg) {
    _sem_timed_ctx_t* ctx = (_sem_timed_ctx_t*)arg;
    xylem_sem_t*      s   = ctx->sem;
    _sem_co_timed_t*  w   = ctx->w;

    w->base.co = co;

    spin_lock(&s->guard);
    /* A post() may have banked a token since the fast path; take it. */
    if (_sem_try(s)) {
        spin_unlock(&s->guard);
        atomic_store(&w->base.granted, 1);
        return false;
    }
    /**
     * The timer may have fired during a re-contend gap (node unlinked) and
     * set timed_out. Decline the park so the coroutine reports the timeout
     * in its loop instead of re-parking on a timer that will never fire
     * again.
     */
    if (atomic_load(&w->timed_out)) {
        spin_unlock(&s->guard);
        return false;
    }

    list_insert_tail(&s->co_waiters, &w->base.node);

    /**
     * Arm under the guard so a post() blocked on it cannot resume this
     * coroutine before the timer is live, and only once across the
     * re-contend loop -- the original deadline is preserved on re-park, so
     * the timer is never restarted. scheduler_timer_start only touches the
     * timer heap, so holding the spin across it is safe.
     */
    if (!w->armed) {
        w->armed = true;
        _sem_co_ref(w); /* timer ref: released by the cb, or by us on stop() */
        if (scheduler_timer_start(
                w->timer, _sem_co_timeout_cb, w, w->timeout_ms, 0) != 0) {
            list_remove(&s->co_waiters, &w->base.node);
            w->armed = false;
            atomic_store(&w->timed_out, true);
            _sem_co_unref(w);
            spin_unlock(&s->guard);
            return false;
        }
    }
    spin_unlock(&s->guard);
    return true;
}

/* Infinite coroutine waiter: stack record, parked via the scheduler. */

typedef struct _sem_inf_ctx_s {
    xylem_sem_t*      sem;
    _sem_co_waiter_t* w;
} _sem_inf_ctx_t;

static bool _sem_inf_park_cb(mco_coro* co, void* arg) {
    _sem_inf_ctx_t* ctx = (_sem_inf_ctx_t*)arg;
    xylem_sem_t*    s   = ctx->sem;

    ctx->w->co = co;

    spin_lock(&s->guard);
    /* A post() may have banked a token since the fast path; take it. */
    if (_sem_try(s)) {
        spin_unlock(&s->guard);
        atomic_store(&ctx->w->granted, 1);
        return false;
    }
    list_insert_tail(&s->co_waiters, &ctx->w->node);
    spin_unlock(&s->guard);
    return true;
}

xylem_sem_t* xylem_sem_create(unsigned int value) {
    xylem_sem_t* s = (xylem_sem_t*)calloc(1, sizeof(xylem_sem_t));
    if (!s) {
        return NULL;
    }
    spin_init(&s->guard);
    atomic_init(&s->count, value);
    atomic_init(&s->thr_waiters, 0);
    list_init(&s->co_waiters);
    return s;
}

void xylem_sem_destroy(xylem_sem_t* s) {
    if (!s) {
        return;
    }
    free(s);
}

/**
 * Thread acquire: barge on the futex word. CAS-decrement a free token or
 * sleep while it is zero; `expected == 0` makes the wait a no-op if a
 * post() banked a token between the load and the sleep. thr_waiters is
 * published atomically so post() cannot skip our wake (the store pairs
 * with post()'s count bump + thr_waiters read).
 */
static bool _sem_wait_thread(xylem_sem_t* s, bool timed, uint64_t timeout_ms) {
    uint64_t deadline = timed ? _sem_now_ms() + timeout_ms : 0;

    atomic_fetch_add(&s->thr_waiters, 1);
    bool got = false;
    for (;;) {
        uint32_t c = atomic_load(&s->count);
        if (c > 0) {
            if (atomic_compare_exchange_weak(&s->count, &c, c - 1)) {
                got = true;
                break;
            }
            continue; /* lost the race for this token; reload and retry */
        }
        if (!timed) {
            platform_futex_wait(&s->count, 0);
            continue;
        }
        uint64_t now = _sem_now_ms();
        if (now >= deadline) {
            break; /* timed out */
        }
        platform_futex_timedwait(&s->count, 0, deadline - now);
    }
    atomic_fetch_sub(&s->thr_waiters, 1);
    return got;
}

void xylem_sem_wait(xylem_sem_t* s) {
    if (mco_running()) {
        /* Coroutine: park on a stack waiter (park cb re-checks the token). */
        if (_sem_try(s)) {
            return;
        }

        _sem_co_waiter_t w;
        w.co    = NULL;
        w.sched = runtime_get_scheduler();
        atomic_init(&w.granted, 0);

        /**
         * Re-park until a post() either hands us a token (granted == 1) or
         * wakes us to re-contend (granted == 0) and we win the banked
         * token; a lost re-contention loops back to re-park.
         */
        _sem_inf_ctx_t ctx = { s, &w };
        for (;;) {
            atomic_store(&w.granted, 0);
            scheduler_park(w.sched, _sem_inf_park_cb, &ctx);
            if (atomic_load(&w.granted)) {
                return; /* token handed over (or taken in the callback) */
            }
            if (_sem_try(s)) {
                return; /* won the banked token */
            }
            /* lost the race; re-park */
        }
    }

    /* Thread: barge on the futex word until a token is taken. */
    (void)_sem_wait_thread(s, false, 0);
}

bool xylem_sem_timedwait(xylem_sem_t* s, uint64_t timeout_ms) {
    /* Zero timeout is a non-blocking try in any context. */
    if (timeout_ms == 0) {
        return _sem_try(s);
    }

    if (mco_running()) {
        if (_sem_try(s)) {
            return true;
        }

        _sem_co_timed_t* w =
            (_sem_co_timed_t*)calloc(1, sizeof(_sem_co_timed_t));
        if (!w) {
            /* Cannot honour a timeout we cannot arm; fail closed. */
            return false;
        }
        w->base.co    = NULL;
        w->base.sched = runtime_get_scheduler();
        w->sem        = s;
        w->timeout_ms = timeout_ms;
        w->timer      = scheduler_timer_create(w->base.sched);
        if (!w->timer) {
            /* No timer means no deadline; fail closed over an unbounded wait. */
            free(w);
            return false;
        }
        atomic_init(&w->base.granted, 0);
        atomic_init(&w->refcnt, 1); /* initial wait ref */
        atomic_init(&w->timed_out, false);

        _sem_timed_ctx_t ctx = { s, w };

        /**
         * Re-park until handed a token (granted == 1), until we win a
         * banked token on a re-contend wake, or until the timer fires
         * (timed_out). A lost re-contention that has not yet timed out
         * loops back to re-park; the timer keeps the original deadline,
         * so it is armed once (see _sem_timed_park_cb) and not restarted.
         */
        bool ok;
        for (;;) {
            atomic_store(&w->base.granted, 0);
            scheduler_park(w->base.sched, _sem_timed_park_cb, &ctx);
            if (atomic_load(&w->base.granted)) {
                ok = true; /* token handed over (or taken in the callback) */
                break;
            }
            if (_sem_try(s)) {
                ok = true; /* won the banked token */
                break;
            }
            if (atomic_load(&w->timed_out)) {
                ok = false; /* deadline elapsed */
                break;
            }
            /* lost the re-contend race, not timed out: re-park */
        }

        /**
         * Cancel a still-pending timer; a true from stop() means we
         * caught it before it fired and own its ref.
         */
        if (scheduler_timer_stop(w->timer)) {
            _sem_co_unref(w);
        }
        _sem_co_unref(w);
        return ok;
    }

    /* External thread with deadline: barge on the futex word. */
    return _sem_wait_thread(s, true, timeout_ms);
}

/**
 * Detach the FIFO-oldest coroutine waiter, recording in `granted` how it
 * was woken (1 = token handed over, 0 = re-contend). Reads the waiter out
 * before the caller schedules it, since its frame (infinite wait) vanishes
 * once resumed.
 */
static mco_coro* _sem_take_waiter(
    xylem_sem_t* s, list_node_t* n, uint32_t granted, scheduler_t** sched) {
    list_remove(&s->co_waiters, n);
    _sem_co_waiter_t* w  = list_entry(n, _sem_co_waiter_t, node);
    mco_coro*         co = w->co;
    *sched               = w->sched;
    atomic_store(&w->granted, granted);
    return co;
}

void xylem_sem_post(xylem_sem_t* s) {
    spin_lock(&s->guard);
    list_node_t* n = list_head(&s->co_waiters);

    /**
     * Pure coroutine hand-off: the token rides the wake, count stays
     * untouched, none is lost -- the zero-syscall fast path. Taken only
     * while no OS thread is blocked, since a thread cannot accept a handed
     * token; it must observe a banked count. The thr_waiters read may race
     * a thread arming right after, which is safe: this branch banks
     * nothing, so the racing thread simply blocks on a genuinely empty
     * count until the next post takes the release path below.
     */
    if (n && atomic_load(&s->thr_waiters)
                == 0) {
        scheduler_t* sched;
        mco_coro*    co = _sem_take_waiter(s, n, 1, &sched);
        spin_unlock(&s->guard);
        scheduler_schedule(sched, co);
        return;
    }

    /**
     * Release path: bank the token under the guard (so a coroutine racing
     * in its park callback either is seen above or observes the banked
     * count), then wake one of each to contend for it -- the FIFO-oldest
     * coroutine to re-contend (granted == 0) and, when a thread is blocked,
     * a futex signal. The default atomic bank pairs with a thread waiter's
     * arm so the wake is never skipped while a thread sleeps on a zero word.
     * Whoever CAS-takes the banked token first wins; the loser re-parks or
     * re-sleeps. This is what keeps threads from starving under a steady
     * stream of coroutine waiters.
     */
    scheduler_t* sched = NULL;
    mco_coro*    co    = n ? _sem_take_waiter(s, n, 0, &sched) : NULL;
    atomic_fetch_add(&s->count, 1);
    spin_unlock(&s->guard);

    if (atomic_load(&s->thr_waiters) > 0) {
        platform_futex_signal(&s->count);
    }
    if (co) {
        scheduler_schedule(sched, co);
    }
}
