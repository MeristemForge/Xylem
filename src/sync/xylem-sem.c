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

#include "container/list.h"
#include "platform/platform-sem.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"
#include "thrds.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Cross-context counting semaphore
 *
 * The one sync primitive both coroutines and plain OS threads may
 * block on, so a coroutine can notify an external thread and vice
 * versa. The other primitives here are coroutine-only; this one is
 * not.
 *
 * State (all under the spin `guard`):
 *   - `count`   token count.
 *   - `waiters` one FIFO list mixing every waiter kind.
 *
 * Each queued waiter records how it is woken, decided by *what it is*,
 * not by who posts:
 *   - SEM_WAITER_CO  : a parked coroutine; woken via scheduler_schedule.
 *   - SEM_WAITER_THR : a blocked OS thread; woken by posting the thread's
 *                      own platform_sem (one per thread, cached in TLS).
 *
 * Direct hand-off: when a waiter is queued, post() pulls the FIFO head
 * and wakes it without touching `count`; the woken waiter returns from
 * wait() with the token already granted, so none is lost.
 *
 * Waiter lifetime:
 *   - Infinite coroutine wait and every thread wait keep the waiter on
 *     the blocked party's stack (the coroutine is suspended, the thread
 *     is in platform_sem_(timed)wait). post() copies the wake target
 *     out under the guard and never dereferences the record afterwards.
 *   - A *timed* coroutine wait needs a scheduler timer that can pull
 *     the waiter out of the queue from another worker. That timer
 *     callback may run concurrently with the resumed coroutine, so a
 *     stack record would be unsafe: the timed coroutine waiter is a
 *     refcounted heap object (wait ref + timer ref), freed by whoever
 *     drops the last reference -- the pattern iowait/channel use.
 */

typedef enum _sem_waiter_kind_e {
    SEM_WAITER_CO,
    SEM_WAITER_THR,
} _sem_waiter_kind_t;

/**
 * Stack waiter: infinite coroutine wait and all thread waits. The timed
 * coroutine waiter embeds this as its first member.
 */
typedef struct _sem_waiter_s {
    list_node_t        node;
    _sem_waiter_kind_t kind;
    mco_coro*          co;    /* SEM_WAITER_CO  */
    scheduler_t*       sched; /* SEM_WAITER_CO  */
    platform_sem_t*    tsem;  /* SEM_WAITER_THR */
} _sem_waiter_t;

struct xylem_sem_s {
    spin_t   guard;
    uint32_t count;
    list_t   waiters;
};

/**
 * A NULL next link means the node was never linked or was removed
 * (list_remove clears it). Read under the guard to arbitrate the
 * timeout-vs-post race.
 */
static inline bool _sem_node_linked(const list_node_t* n) {
    return n->next != NULL;
}

/* Per-thread wake semaphore (TLS, reused for the thread's lifetime). */

static tss_t     _sem_tls_key;
static once_flag _sem_tls_once = ONCE_FLAG_INIT;

static void _sem_tls_dtor(void* p) {
    if (p) {
        platform_sem_destroy((platform_sem_t*)p);
    }
}

static void _sem_tls_init(void) {
    tss_create(&_sem_tls_key, _sem_tls_dtor);
}

static platform_sem_t* _sem_thread_sem(void) {
    call_once(&_sem_tls_once, _sem_tls_init);

    platform_sem_t* sem = (platform_sem_t*)tss_get(_sem_tls_key);
    if (!sem) {
        sem = platform_sem_create(0);
        if (sem) {
            tss_set(_sem_tls_key, sem);
        }
    }
    return sem;
}

/* Timed coroutine waiter: refcounted heap object. */

typedef struct _sem_co_timed_s {
    _sem_waiter_t   base;
    xylem_sem_t*    sem;
    sched_timer_t*  timer;      /* NULL if creation failed */
    uint64_t        timeout_ms;
    _Atomic int32_t refcnt;     /* wait ref + timer ref while armed */
    _Atomic bool    timed_out;
} _sem_co_timed_t;

static void _sem_co_unref(_sem_co_timed_t* w) {
    if (atomic_fetch_sub_explicit(&w->refcnt, 1, memory_order_acq_rel) != 1) {
        return;
    }
    /* The last ref owns the timer; destroy is safe even mid-callback. */
    if (w->timer) {
        sched_timer_destroy(w->timer);
    }
    free(w);
}

static void _sem_co_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    _sem_co_timed_t* w = (_sem_co_timed_t*)ud;
    xylem_sem_t*     s = w->sem;

    /**
     * If the node is already gone, a post() won the hand-off and will
     * resume the coroutine; scheduling it again would be a double wake.
     */
    spin_lock(&s->guard);
    bool linked = _sem_node_linked(&w->base.node);
    if (linked) {
        list_remove(&s->waiters, &w->base.node);
        atomic_store_explicit(&w->timed_out, true, memory_order_relaxed);
    }
    scheduler_t* sched = w->base.sched;
    mco_coro*    co    = w->base.co;
    spin_unlock(&s->guard);

    /* Last touch of co: it may drop its wait ref and free w on resume. */
    if (linked) {
        scheduler_schedule(sched, co);
    }
    _sem_co_unref(w);
}

static bool _sem_co_timed_park_cb(mco_coro* co, void* arg) {
    _sem_co_timed_t* w = (_sem_co_timed_t*)arg;
    xylem_sem_t*     s = w->sem;

    w->base.co = co;

    spin_lock(&s->guard);
    /* A post() may have bumped the count since the fast path; take it. */
    if (s->count > 0) {
        s->count--;
        spin_unlock(&s->guard);
        return false;
    }

    list_insert_tail(&s->waiters, &w->base.node);

    /**
     * Arm under the guard so a post() blocked on it cannot resume this
     * coroutine before the timer is live. sched_timer_start only
     * touches the timer heap, so holding the spin across it is safe.
     */
    atomic_fetch_add_explicit(&w->refcnt, 1, memory_order_relaxed);
    sched_timer_start(w->timer, _sem_co_timeout_cb, w, w->timeout_ms, 0);
    spin_unlock(&s->guard);
    return true;
}

/* Infinite coroutine waiter: stack record, parked via the scheduler. */

typedef struct _sem_inf_ctx_s {
    xylem_sem_t*   sem;
    _sem_waiter_t* w;
} _sem_inf_ctx_t;

static bool _sem_inf_park_cb(mco_coro* co, void* arg) {
    _sem_inf_ctx_t* ctx = (_sem_inf_ctx_t*)arg;
    xylem_sem_t*    s   = ctx->sem;

    ctx->w->co = co;

    spin_lock(&s->guard);
    /* A post() may have bumped the count since the fast path; take it. */
    if (s->count > 0) {
        s->count--;
        spin_unlock(&s->guard);
        return false;
    }
    list_insert_tail(&s->waiters, &ctx->w->node);
    spin_unlock(&s->guard);
    return true;
}

xylem_sem_t* xylem_sem_create(unsigned int value) {
    xylem_sem_t* s = (xylem_sem_t*)calloc(1, sizeof(xylem_sem_t));
    if (!s) {
        return NULL;
    }
    spin_init(&s->guard);
    s->count = value;
    list_init(&s->waiters);
    return s;
}

void xylem_sem_destroy(xylem_sem_t* s) {
    if (!s) {
        return;
    }
    free(s);
}

/* Non-blocking token grab, identical in any context. */
static bool _sem_try(xylem_sem_t* s) {
    spin_lock(&s->guard);
    if (s->count > 0) {
        s->count--;
        spin_unlock(&s->guard);
        return true;
    }
    spin_unlock(&s->guard);
    return false;
}

void xylem_sem_wait(xylem_sem_t* s) {
    if (mco_running()) {
        /* Coroutine: park on a stack waiter (park cb re-checks count). */
        if (_sem_try(s)) {
            return;
        }

        _sem_waiter_t w;
        w.kind  = SEM_WAITER_CO;
        w.sched = runtime_get_scheduler();

        _sem_inf_ctx_t ctx = { s, &w };
        scheduler_park(w.sched, _sem_inf_park_cb, &ctx);
        return;
    }

    /* External thread, no deadline: block on the per-thread OS sem. */
    spin_lock(&s->guard);
    if (s->count > 0) {
        s->count--;
        spin_unlock(&s->guard);
        return;
    }
    _sem_waiter_t w;
    w.kind = SEM_WAITER_THR;
    w.tsem = _sem_thread_sem();
    if (!w.tsem) {
        /* No wake sem means we could never be woken and post() would
         * deref a NULL tsem; do not enqueue. Fail open over a wait we
         * cannot satisfy (OOM only). */
        spin_unlock(&s->guard);
        return;
    }
    list_insert_tail(&s->waiters, &w.node);
    spin_unlock(&s->guard);

    platform_sem_wait(w.tsem);
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
        w->base.kind  = SEM_WAITER_CO;
        w->base.sched = runtime_get_scheduler();
        w->sem        = s;
        w->timeout_ms = timeout_ms;
        w->timer      = sched_timer_create(w->base.sched);
        if (!w->timer) {
            /* No timer means no deadline; fail closed over an unbounded wait. */
            free(w);
            return false;
        }
        atomic_init(&w->refcnt, 1);
        atomic_init(&w->timed_out, false);

        scheduler_park(w->base.sched, _sem_co_timed_park_cb, w);

        /**
         * Woken by post() (timed_out false) or the timer (true). Cancel
         * a still-pending timer; a true from stop() means we caught it
         * before it fired and own its ref.
         */
        if (sched_timer_stop(w->timer)) {
            _sem_co_unref(w);
        }
        bool ok = !atomic_load_explicit(&w->timed_out, memory_order_acquire);
        _sem_co_unref(w);
        return ok;
    }

    /* External thread with deadline. */
    spin_lock(&s->guard);
    if (s->count > 0) {
        s->count--;
        spin_unlock(&s->guard);
        return true;
    }
    _sem_waiter_t w;
    w.kind = SEM_WAITER_THR;
    w.tsem = _sem_thread_sem();
    if (!w.tsem) {
        /* No wake sem means we could never be woken and post() would
         * deref a NULL tsem; do not enqueue. Fail closed over a wait we
         * cannot satisfy (OOM only). */
        spin_unlock(&s->guard);
        return false;
    }
    list_insert_tail(&s->waiters, &w.node);
    spin_unlock(&s->guard);

    if (platform_sem_timedwait(w.tsem, timeout_ms) == 0) {
        return true; /* a post() handed us the token */
    }

    /* Timed out: resolve a post() that may have dequeued us concurrently. */
    spin_lock(&s->guard);
    if (_sem_node_linked(&w.node)) {
        list_remove(&s->waiters, &w.node);
        spin_unlock(&s->guard);
        return false;
    }
    spin_unlock(&s->guard);

    /**
     * A post() already dequeued us and will post our tsem after
     * releasing the guard. Consume that token so it is not lost and
     * report success.
     */
    platform_sem_wait(w.tsem);
    return true;
}

void xylem_sem_post(xylem_sem_t* s) {
    spin_lock(&s->guard);
    list_node_t* n = list_head(&s->waiters);
    if (!n) {
        s->count++;
        spin_unlock(&s->guard);
        return;
    }
    list_remove(&s->waiters, n);

    /**
     * Snapshot the wake target before dropping the guard: a timed heap
     * waiter may resume on another worker and free the record once the
     * guard is released, so the record must not be touched after.
     */
    _sem_waiter_t*     w     = list_entry(n, _sem_waiter_t, node);
    _sem_waiter_kind_t kind  = w->kind;
    mco_coro*          co    = w->co;
    scheduler_t*       sched = w->sched;
    platform_sem_t*    tsem  = w->tsem;
    spin_unlock(&s->guard);

    if (kind == SEM_WAITER_CO) {
        scheduler_schedule(sched, co);
    } else {
        platform_sem_post(tsem);
    }
}
