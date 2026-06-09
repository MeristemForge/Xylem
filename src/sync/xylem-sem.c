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

#include "container/queue.h"
#include "platform/platform-sem.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"
#include "thrds.h"

#include "runtime/minicoro/minicoro.h"

#include <stdbool.h>
#include <stdlib.h>

/**
 * Cross-context counting semaphore
 *
 * This is the one sync primitive that both coroutines and plain OS
 * threads may block on, so that a coroutine can notify an external
 * thread and vice-versa. The other primitives in this directory are
 * coroutine-only and abort off-coroutine; this one is not.
 *
 * State:
 *   - `count` is the token count, guarded by the spin `guard`.
 *   - `waiters` is a single FIFO list mixing both waiter kinds. One
 *     spin section serialises every count/list mutation, so there is
 *     no missed-wakeup window.
 *
 * Each waiter records *how it is woken*, decided by what it is, not by
 * who posts:
 *   - WAITER_CO  : a parked coroutine; woken with scheduler_schedule on
 *                  the scheduler it parked under.
 *   - WAITER_THR : a blocked OS thread; woken by posting the thread's
 *                  own platform_sem (one per thread, cached in TLS).
 *
 * Direct hand-off: when a waiter is queued, post() transfers the token
 * straight to the FIFO-oldest waiter and never touches `count`. The
 * woken waiter returns from wait() without re-decrementing, so a token
 * can never be lost between post and the waiter's resume.
 *
 * The waiter record lives on the blocked party's stack (the coroutine
 * is suspended; the thread is blocked in platform_sem_wait), so it
 * stays valid until that party resumes. post() copies out the wake
 * target under the guard before releasing it and never dereferences
 * the record afterwards.
 */

typedef enum _sem_waiter_kind_e {
    WAITER_CO,
    WAITER_THR,
} _sem_waiter_kind_t;

typedef struct _sem_waiter_s {
    queue_node_t       node;
    _sem_waiter_kind_t kind;
    /* WAITER_CO */
    mco_coro*          co;
    scheduler_t*       sched;
    /* WAITER_THR */
    platform_sem_t*    tsem;
} _sem_waiter_t;

struct xylem_sem_s {
    spin_t       guard;
    unsigned int count;
    queue_t      waiters;
};

/* Per-thread wake semaphore, lazily created on first thread-side wait
 * and reused for the life of the thread; reclaimed by the TLS dtor. */
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

typedef struct _sem_park_ctx_s {
    xylem_sem_t*  sem;
    _sem_waiter_t waiter;
} _sem_park_ctx_t;

static bool _sem_park_cb(mco_coro* co, void* arg) {
    _sem_park_ctx_t* ctx = (_sem_park_ctx_t*)arg;
    xylem_sem_t*     s   = ctx->sem;

    ctx->waiter.co = co;

    spin_lock(&s->guard);
    /* A post() between the fast-path check and here may have handed a
     * token to the count (no waiter was queued yet). Take it and skip
     * parking. */
    if (s->count > 0) {
        s->count--;
        spin_unlock(&s->guard);
        return false;
    }
    queue_enqueue(&s->waiters, &ctx->waiter.node);
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
    queue_init(&s->waiters);
    return s;
}

void xylem_sem_destroy(xylem_sem_t* s) {
    if (!s) {
        return;
    }
    free(s);
}

bool xylem_sem_trywait(xylem_sem_t* s) {
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
        /* Coroutine: take a token on the fast path, else park. The
         * park callback re-checks the count under the guard and only
         * enqueues if still empty, closing the race with a concurrent
         * post(). */
        spin_lock(&s->guard);
        if (s->count > 0) {
            s->count--;
            spin_unlock(&s->guard);
            return;
        }
        spin_unlock(&s->guard);

        _sem_park_ctx_t ctx;
        ctx.sem           = s;
        ctx.waiter.kind   = WAITER_CO;
        ctx.waiter.sched  = runtime_get_scheduler();
        scheduler_park(ctx.waiter.sched, _sem_park_cb, &ctx);
        return;
    }

    /* External thread: take a token or block on a per-thread OS
     * semaphore. The whole decision happens under the guard so a
     * concurrent post() either hands us the count or finds us queued. */
    spin_lock(&s->guard);
    if (s->count > 0) {
        s->count--;
        spin_unlock(&s->guard);
        return;
    }

    _sem_waiter_t w;
    w.kind = WAITER_THR;
    w.tsem = _sem_thread_sem();
    queue_enqueue(&s->waiters, &w.node);
    spin_unlock(&s->guard);

    platform_sem_wait(w.tsem);
}

void xylem_sem_post(xylem_sem_t* s) {
    spin_lock(&s->guard);
    queue_node_t* n = queue_dequeue(&s->waiters);
    if (!n) {
        s->count++;
        spin_unlock(&s->guard);
        return;
    }

    /* Copy the wake target out before dropping the guard: once the
     * waiter resumes, its stack-resident record is gone. */
    _sem_waiter_t*     w     = queue_entry(n, _sem_waiter_t, node);
    _sem_waiter_kind_t kind  = w->kind;
    mco_coro*          co    = w->co;
    scheduler_t*       sched = w->sched;
    platform_sem_t*    tsem  = w->tsem;
    spin_unlock(&s->guard);

    if (kind == WAITER_CO) {
        scheduler_schedule(sched, co);
    } else {
        platform_sem_post(tsem);
    }
}
