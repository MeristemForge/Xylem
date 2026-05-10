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

/**
 * Scheduling model
 * ----------------
 *
 * N worker threads cooperate through a three-tier runnable pool:
 *
 *   1. per-worker `runnext` slot   - single LIFO hand-off, cache-hot
 *   2. per-worker work-stealing    - owner pushes/pops the tail, other
 *      deque (wsdeque)               workers steal from the head
 *   3. global runq (mpsc)          - overflow from full deques, and
 *                                    injection point for cross-thread
 *                                    scheduler_schedule() callers
 *
 * When a worker runs out of local work it spins a few times (non-
 * blocking polls + steals). The last remaining spinner does a
 * blocking poll that also processes timers and deferred posts, so
 * IO/timers/posts never starve even when all other workers are idle.
 * Workers without work eventually park on a per-worker semaphore;
 * scheduler_schedule wakes one parked worker on each push.
 *
 * Coroutine parking flows through scheduler_park: the park callback
 * is invoked *after* mco_yield returns, so a wakeup source can never
 * observe the coroutine pointer before the yield has actually
 * suspended it. iowait.c relies on this to avoid schedule-before-
 * yield races.
 */

#include "scheduler.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "iowait.h"
#include "wsdeque.h"
#include "runq.h"
#include "container/heap.h"
#include "container/list.h"
#include "container/mpsc.h"
#include "container/queue.h"
#include "platform/platform-sem.h"
#include "platform/platform-socket.h"
#include "platform/platform-info.h"
#include "sync/spin.h"
#include "thrds.h"

#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SCHED_DEFAULT_DEQUE_LOG2 10
#define SCHED_DEQUE_HALF         (1 << (SCHED_DEFAULT_DEQUE_LOG2 - 1))
#define SCHED_CORO_STACK_SIZE    131072
#define SCHED_MAX_POLL_MS        5
#define SCHED_SPIN_ATTEMPTS      4
#define SCHED_RUNQ_GRAB_MAX      32
#define SCHED_TIMER_TICK_MS      1

typedef struct _sched_worker_s {
    thrd_t               thread;
    wsdeque_t*           deque;
    platform_sem_t*      sem;
    scheduler_t*         sched;
    uint32_t             index;
    scheduler_park_fn_t  park_fn;
    void*                park_arg;
    _Atomic bool         parked;
    _Atomic(mco_coro*)   runnext;
} _sched_worker_t;

struct scheduler_s {
    _sched_worker_t*      workers;
    int32_t               nworkers;
    runq_t*               runq;
    heap_t                timers;
    mtx_t                 timer_lock;
    mpsc_t                posts;
    platform_poller_sq_t  poller;
    platform_poller_sqe_t wakeup_sqe;
    platform_sock_t       wakeup_rd;
    platform_sock_t       wakeup_wr;
    iowait_pool_t*        iowait_pool;
    scheduler_idle_fn_t   idle_cb;
    void*                 idle_ud;
    _Atomic bool          processing;
    _Atomic bool          running;
    bool                  joined;
    _Atomic int32_t       nspinning;
    _Atomic int32_t       nparked;
    _Atomic int64_t       alive;
    _Atomic uint64_t      timer_last_tick_ms;
    /**
     * Covers coroutines parked on channels/mutexes/iowait/timers
     * which are not reachable through runq/deque/runnext and would
     * otherwise leak on destroy. Spin lock because critical sections
     * are pure list ops and never park.
     */
    list_t                registry;
    spin_t                registry_lock;
};

static thread_local _sched_worker_t* _tls_worker;

typedef struct {
    void (*fn)(void*);
    void*        arg;
    queue_node_t runq_node;
    list_node_t  registry_node;
    mco_coro*    co;
} _coro_ctx_t;

typedef struct {
    mpsc_node_t          node;
    scheduler_post_fn_t  cb;
    void*                ud;
} _sched_post_t;

/**
 * sched_timer_s
 *
 * refcnt pins the timer across the in-flight-fire window so a racing
 * sched_timer_destroy can drop the creator's ref without freeing the
 * object out from under the still-running callback.
 */
struct sched_timer_s {
    heap_node_t      heap_node;
    scheduler_t*     sched;
    sched_timer_fn_t cb;
    void*            ud;
    uint64_t         timeout;
    uint64_t         repeat;
    bool             active;
    _Atomic int32_t  refcnt;
};

static void _sched_coro_entry(mco_coro* co) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
    ctx->fn(ctx->arg);
}

static void _sched_timer_ref(sched_timer_t* timer) {
    atomic_fetch_add_explicit(&timer->refcnt, 1, memory_order_relaxed);
}

static void _sched_timer_unref(sched_timer_t* timer) {
    if (atomic_fetch_sub_explicit(
            &timer->refcnt, 1, memory_order_acq_rel) == 1) {
        free(timer);
    }
}

static int _sched_timer_cmp(
    const heap_node_t* a, const heap_node_t* b) {
    const sched_timer_t* ta = heap_entry(a, sched_timer_t, heap_node);
    const sched_timer_t* tb = heap_entry(b, sched_timer_t, heap_node);
    if (ta->timeout < tb->timeout) {
        return -1;
    }
    if (ta->timeout > tb->timeout) {
        return 1;
    }
    return 0;
}

static void _sched_wake_poller(scheduler_t* sched) {
    if (sched->wakeup_wr) {
        char c = 1;
        platform_socket_send(sched->wakeup_wr, &c, 1);
    }
}

/* Wake one parked worker; if none, poke the poller to unblock epoll_wait. */
static void _sched_wake_worker(scheduler_t* sched) {
    for (int32_t i = 0; i < sched->nworkers; i++) {
        if (atomic_load(&sched->workers[i].parked)) {
            platform_sem_post(sched->workers[i].sem);
            return;
        }
    }
    _sched_wake_poller(sched);
}

static mco_coro* _sched_try_get_coro(scheduler_t* sched, _sched_worker_t* w) {
    /* runnext first: LIFO hand-off is cache-hot. */
    mco_coro* co = atomic_exchange(&w->runnext, NULL);
    if (co) {
        return co;
    }

    co = wsdeque_pop(w->deque);
    if (co) {
        return co;
    }

    {
        /* Fair share from global runq to avoid one worker monopolizing it. */
        queue_node_t* nodes[SCHED_RUNQ_GRAB_MAX];
        int32_t n = runq_pop_batch(
            sched->runq, nodes, SCHED_RUNQ_GRAB_MAX / sched->nworkers + 1);
        if (n > 0) {
            for (int32_t i = 1; i < n; i++) {
                _coro_ctx_t* c = queue_entry(nodes[i], _coro_ctx_t, runq_node);
                wsdeque_push(w->deque, c->co);
            }
            _coro_ctx_t* ctx = queue_entry(nodes[0], _coro_ctx_t, runq_node);
            return ctx->co;
        }
    }

    if (sched->nworkers > 1) {
        uint32_t start = w->index + 1;
        for (int32_t i = 0; i < sched->nworkers - 1; i++) {
            uint32_t idx = (start + (uint32_t)i) % (uint32_t)sched->nworkers;
            mco_coro* batch[SCHED_DEQUE_HALF];
            int32_t n = wsdeque_steal_half(
                sched->workers[idx].deque, batch, SCHED_DEQUE_HALF);
            if (n > 0) {
                for (int32_t j = 1; j < n; j++) {
                    wsdeque_push(w->deque, batch[j]);
                }
                return batch[0];
            }
        }
    }

    return NULL;
}

/* ms until the earliest timer fires, lock held. -1 if empty. */
static int _sched_timeout_locked(scheduler_t* sched, uint64_t now) {
    heap_node_t* root = heap_peek(&sched->timers);
    if (!root) {
        return -1;
    }
    sched_timer_t* t = heap_entry(root, sched_timer_t, heap_node);
    if (t->timeout <= now) {
        return 0;
    }
    uint64_t diff = t->timeout - now;
    return (diff > INT32_MAX) ? INT32_MAX : (int)diff;
}

static int _sched_timer_next_timeout(scheduler_t* sched) {
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    mtx_lock(&sched->timer_lock);
    int timeout = _sched_timeout_locked(sched, now);
    mtx_unlock(&sched->timer_lock);
    return timeout;
}

static int _sched_process_timers(scheduler_t* sched, uint64_t now_ms) {
    for (;;) {
        sched_timer_t*   timer = NULL;
        sched_timer_fn_t cb    = NULL;
        void*            ud    = NULL;

        mtx_lock(&sched->timer_lock);
        heap_node_t* root = heap_peek(&sched->timers);
        if (root) {
            sched_timer_t* t = heap_entry(root, sched_timer_t, heap_node);
            if (t->timeout <= now_ms) {
                heap_dequeue(&sched->timers);
                if (t->repeat > 0) {
                    t->timeout = now_ms + t->repeat;
                    heap_insert(&sched->timers, &t->heap_node);
                } else {
                    t->active = false;
                }
                /**
                 * Snapshot cb/ud under lock so a concurrent start()
                 * cannot swap the callback between dequeue and
                 * invocation; a racing start() is simply serviced on
                 * the next pass.
                 *
                 * Pin the timer across the unlocked cb call so a
                 * racing destroy that drops the creator's ref does
                 * not free the object while cb is running.
                 */
                _sched_timer_ref(t);
                timer = t;
                cb    = t->cb;
                ud    = t->ud;
            }
        }
        mtx_unlock(&sched->timer_lock);

        if (!timer) {
            break;
        }

        cb(timer, ud);

        _sched_timer_unref(timer);
    }

    mtx_lock(&sched->timer_lock);
    int timeout = _sched_timeout_locked(sched, now_ms);
    mtx_unlock(&sched->timer_lock);
    return timeout;
}

static void _sched_process_posts(scheduler_t* sched) {
    mpsc_node_t* node;
    while ((node = mpsc_pop(&sched->posts)) != NULL) {
        _sched_post_t* req = mpsc_entry(node, _sched_post_t, node);
        req->cb(req->ud);
        free(req);
    }
}

static void _sched_process_io(
    scheduler_t* sched,
    platform_poller_cqe_t* cqes,
    int n) {
    for (int i = 0; i < n; i++) {
        /**
         * ud == NULL is the sentinel for our own wakeup pipe; every
         * other registration goes through iowait with a
         * generation-tagged ud.
         */
        if (cqes[i].ud == NULL) {
            /* Drain so the pipe can be re-triggered. */
            char buf[64];
            while (platform_socket_recv(sched->wakeup_rd, buf, sizeof(buf)) > 0) {
            }
            if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
                sched->wakeup_sqe.op = PLATFORM_POLLER_RD_OP;
                platform_poller_mod(&sched->poller, &sched->wakeup_sqe);
            }
            continue;
        }
        iowait_on_event((int)cqes[i].op, cqes[i].ud);
    }
}

static void _sched_handle_yield(_sched_worker_t* w, mco_coro* co) {
    if (mco_status(co) == MCO_DEAD) {
        _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);

        spin_lock(&w->sched->registry_lock);
        list_remove(&w->sched->registry, &ctx->registry_node);
        spin_unlock(&w->sched->registry_lock);

        free(ctx);
        mco_destroy(co);

        scheduler_t* sched = w->sched;
        int64_t prev = atomic_fetch_sub(&sched->alive, 1);
        if (prev == 1 && sched->idle_cb) {
            sched->idle_cb(sched->idle_ud);
        }
        return;
    }
    if (w->park_fn) {
        scheduler_park_fn_t fn = w->park_fn;
        void* arg = w->park_arg;
        w->park_fn  = NULL;
        w->park_arg = NULL;
        if (!fn(co, arg)) {
            /* Park declined: prefer local deque, fall back to global
               runq on overflow so we never drop a live coroutine. */
            if (wsdeque_push(w->deque, co) != 0) {
                _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
                runq_push(w->sched->runq, &ctx->runq_node);
                _sched_wake_worker(w->sched);
            }
        }
    }
}

static inline void _sched_run_coro(_sched_worker_t* w, mco_coro* co) {
    mco_resume(co);
    _sched_handle_yield(w, co);
}

/**
 * Spin with non-blocking polls looking for a runnable coroutine.
 * Returns NULL if all attempts exhausted.
 */
static mco_coro* _sched_try_spin(
    scheduler_t* sched,
    _sched_worker_t* w,
    platform_poller_cqe_t* cqes) {
    for (int spin = 0; spin < SCHED_SPIN_ATTEMPTS; spin++) {
        int n = platform_poller_wait(&sched->poller, cqes, 0);
        if (n > 0) {
            _sched_process_io(sched, cqes, n);
        }

        mco_coro* co = _sched_try_get_coro(sched, w);
        if (co) {
            return co;
        }
    }
    return NULL;
}

/**
 * Blocking poll loop for the last spinner. Services IO, timers, and
 * posts so they cannot be starved when every other worker is idle.
 */
static mco_coro* _sched_poll_blocking(
    scheduler_t* sched,
    _sched_worker_t* w,
    platform_poller_cqe_t* cqes) {
    for (;;) {
        if (!atomic_load(&sched->running)) {
            break;
        }

        int poll_ms = _sched_timer_next_timeout(sched);
        if (poll_ms < 0 || poll_ms > SCHED_MAX_POLL_MS) {
            poll_ms = SCHED_MAX_POLL_MS;
        }
        int n = platform_poller_wait(&sched->poller, cqes, poll_ms);
        if (n > 0) {
            _sched_process_io(sched, cqes, n);
        }

        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        _sched_process_timers(sched, now);

        bool expected = false;
        if (atomic_compare_exchange_strong(
                &sched->processing, &expected, true)) {
            _sched_process_posts(sched);
            atomic_store(&sched->processing, false);
        }

        mco_coro* co = _sched_try_get_coro(sched, w);
        if (co) {
            return co;
        }

        /* Another worker took over spinning; we can stop. */
        if (atomic_load(&sched->nspinning) > 0) {
            break;
        }
    }
    return NULL;
}

static void _sched_drain(_sched_worker_t* w, scheduler_t* sched) {
    for (;;) {
        mco_coro* co = _sched_try_get_coro(sched, w);
        if (!co) {
            break;
        }
        _sched_run_coro(w, co);
    }
}

/**
 * Cooperative timer tick on the fast path.
 *
 * Without this, a CPU-bound workload that keeps every worker busy on
 * runnable coroutines never reaches _sched_poll_blocking, so timers
 * (runtime_sleep, iowait deadlines) would starve indefinitely.
 *
 * The CAS on timer_last_tick_ms elects a single worker per window,
 * so the hot path for all other workers is just one atomic load.
 */
static void _sched_timer_tick(scheduler_t* sched) {
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    uint64_t last = atomic_load_explicit(
        &sched->timer_last_tick_ms, memory_order_relaxed);
    if (now - last < SCHED_TIMER_TICK_MS) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &sched->timer_last_tick_ms,
            &last,
            now,
            memory_order_acq_rel,
            memory_order_relaxed)) {
        return;
    }
    _sched_process_timers(sched, now);
}

static int _sched_worker_entry(void* arg) {
    _sched_worker_t* w = (_sched_worker_t*)arg;
    scheduler_t* sched = w->sched;
    _tls_worker = w;

    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];

    while (atomic_load(&sched->running)) {
        _sched_timer_tick(sched);

        mco_coro* co = _sched_try_get_coro(sched, w);

        if (co) {
            _sched_run_coro(w, co);
            continue;
        }

        atomic_fetch_add(&sched->nspinning, 1);

        co = _sched_try_spin(sched, w, cqes);
        if (co) {
            atomic_fetch_sub(&sched->nspinning, 1);
            _sched_run_coro(w, co);
            continue;
        }

        /* Last spinner does a blocking poll so IO is never missed. */
        int32_t prev = atomic_fetch_sub(&sched->nspinning, 1);
        if (prev == 1) {
            co = _sched_poll_blocking(sched, w, cqes);
            if (co) {
                _sched_run_coro(w, co);
            }
            continue;
        }

        atomic_store(&w->parked, true);
        atomic_fetch_add(&sched->nparked, 1);
        platform_sem_wait(w->sem);
        atomic_store(&w->parked, false);
        atomic_fetch_sub(&sched->nparked, 1);
    }

    _sched_drain(w, sched);
    return 0;
}

static void _sched_cleanup(scheduler_t* sched, int32_t nstarted) {
    atomic_store(&sched->running, false);

    if (sched->workers) {
        if (!sched->joined) {
            /* Wake both parked (sem) and polling (wakeup pipe) workers. */
            for (int32_t i = 0; i < nstarted; i++) {
                platform_sem_post(sched->workers[i].sem);
                _sched_wake_poller(sched);
            }
            for (int32_t i = 0; i < nstarted; i++) {
                thrd_join(sched->workers[i].thread, NULL);
            }
            sched->joined = true;
        }
        for (int32_t i = 0; i < sched->nworkers; i++) {
            if (sched->workers[i].deque) {
                wsdeque_destroy(sched->workers[i].deque);
            }
            if (sched->workers[i].sem) {
                platform_sem_destroy(sched->workers[i].sem);
            }
        }
        free(sched->workers);
    }

    if (sched->runq) {
        runq_destroy(sched->runq);
    }

    /**
     * Force-drain timers still armed at shutdown (for instance a
     * runtime_sleep whose coroutine was abandoned by _sched_drain)
     * so their sched_timer_t objects do not leak. Unref rather than
     * free because an in-flight callback holds its own reference;
     * the last unref is what actually frees.
     */
    {
        mtx_lock(&sched->timer_lock);
        heap_node_t* n;
        while ((n = heap_peek(&sched->timers)) != NULL) {
            sched_timer_t* t = heap_entry(n, sched_timer_t, heap_node);
            heap_dequeue(&sched->timers);
            t->active = false;
            mtx_unlock(&sched->timer_lock);
            _sched_timer_unref(t);
            mtx_lock(&sched->timer_lock);
        }
        mtx_unlock(&sched->timer_lock);
    }

    mtx_destroy(&sched->timer_lock);

    _sched_process_posts(sched);

    if (sched->wakeup_rd) {
        platform_poller_del(&sched->poller, &sched->wakeup_sqe);
        platform_socket_close(sched->wakeup_rd);
        platform_socket_close(sched->wakeup_wr);
    }

    platform_poller_deinit(&sched->poller);
    iowait_pool_destroy(sched->iowait_pool);
    free(sched);
}

scheduler_t* scheduler_create(scheduler_opts_t* opts) {
    scheduler_t* sched = (scheduler_t*)calloc(1, sizeof(scheduler_t));
    if (!sched) {
        return NULL;
    }

    int32_t nworkers = (int32_t)platform_info_getcpus();
    if (nworkers < 1) {
        nworkers = 4;
    }

    uint32_t deque_log2 = SCHED_DEFAULT_DEQUE_LOG2;

    if (opts) {
        if (opts->nworkers > 0) {
            nworkers = opts->nworkers;
        }
        if (opts->deque_cap > 0) {
            deque_log2 = opts->deque_cap;
        }
    }

    sched->runq = runq_create();
    if (!sched->runq) {
        _sched_cleanup(sched, 0);
        return NULL;
    }

    heap_init(&sched->timers, _sched_timer_cmp);
    mtx_init(&sched->timer_lock, mtx_plain);
    mpsc_init(&sched->posts);
    list_init(&sched->registry);
    spin_init(&sched->registry_lock);

    platform_poller_init(&sched->poller);
    {
        platform_sock_t pair[2];
        if (platform_socket_socketpair(0, SOCK_STREAM, 0, pair) == 0) {
            sched->wakeup_rd = pair[0];
            sched->wakeup_wr = pair[1];
            platform_socket_enable_nonblocking(sched->wakeup_rd, true);
            platform_socket_enable_nonblocking(sched->wakeup_wr, true);

            memset(&sched->wakeup_sqe, 0, sizeof(sched->wakeup_sqe));
            sched->wakeup_sqe.fd = (platform_poller_fd_t)sched->wakeup_rd;
            sched->wakeup_sqe.op = PLATFORM_POLLER_RD_OP;
            sched->wakeup_sqe.ud = NULL;
            platform_poller_add(&sched->poller, &sched->wakeup_sqe);
        }
    }

    atomic_store(&sched->running, true);

    sched->iowait_pool = iowait_pool_create();
    if (!sched->iowait_pool) {
        _sched_cleanup(sched, 0);
        return NULL;
    }

    sched->nworkers = nworkers;
    sched->workers = (_sched_worker_t*)calloc(
        (size_t)nworkers, sizeof(_sched_worker_t));
    if (!sched->workers) {
        _sched_cleanup(sched, 0);
        return NULL;
    }

    for (int32_t i = 0; i < nworkers; i++) {
        _sched_worker_t* w = &sched->workers[i];
        w->deque = wsdeque_create(deque_log2);
        w->sem = platform_sem_create(0);
        w->sched = sched;
        w->index = (uint32_t)i;

        if (!w->deque || !w->sem) {
            _sched_cleanup(sched, 0);
            return NULL;
        }
    }

    for (int32_t i = 0; i < nworkers; i++) {
        if (thrd_create(&sched->workers[i].thread,
                        _sched_worker_entry,
                        &sched->workers[i]) != thrd_success) {
            _sched_cleanup(sched, i);
            return NULL;
        }
    }

    return sched;
}

void scheduler_destroy(scheduler_t* sched) {
    if (!sched) {
        return;
    }

    scheduler_stop(sched);

    /**
     * Reclaim coroutine shells still alive at shutdown. Only registry
     * sees coros parked on channels/mutexes/iowait/timers, which are
     * not reachable through runq/deque/runnext.
     *
     * Shell-only reclaim: heap objects referenced from a parked
     * coroutine's stack are not walked, as that would require a
     * cancellation protocol in every parking primitive.
     */
    spin_lock(&sched->registry_lock);
    while (!list_empty(&sched->registry)) {
        list_node_t* n = list_head(&sched->registry);
        list_remove(&sched->registry, n);
        spin_unlock(&sched->registry_lock);

        _coro_ctx_t* ctx = list_entry(n, _coro_ctx_t, registry_node);
        mco_destroy(ctx->co);
        free(ctx);

        spin_lock(&sched->registry_lock);
    }
    spin_unlock(&sched->registry_lock);

    _sched_cleanup(sched, sched->nworkers);
}

void scheduler_stop(scheduler_t* sched) {
    if (!sched || sched->joined) {
        return;
    }

    atomic_store(&sched->running, false);
    for (int32_t i = 0; i < sched->nworkers; i++) {
        platform_sem_post(sched->workers[i].sem);
        _sched_wake_poller(sched);
    }
    for (int32_t i = 0; i < sched->nworkers; i++) {
        thrd_join(sched->workers[i].thread, NULL);
    }
    sched->joined = true;
}

void scheduler_schedule(scheduler_t* sched, mco_coro* co) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);

    /**
     * Fast path requires `sched == _tls_worker->sched` so a process
     * running more than one scheduler still dispatches cross-
     * scheduler calls via the slow path.
     */
    if (_tls_worker && _tls_worker->sched == sched) {
        mco_coro* old = atomic_exchange(&_tls_worker->runnext, co);
        if (!old) {
            return;
        }
        /* runnext occupied; try local deque. */
        if (wsdeque_push(_tls_worker->deque, old) == 0) {
            return;
        }
        /* Deque full: drain half to global runq, then retry. */
        mco_coro* batch[SCHED_DEQUE_HALF];
        int32_t n = wsdeque_pop_half(_tls_worker->deque, batch, SCHED_DEQUE_HALF);
        if (n > 0) {
            queue_node_t* nodes[SCHED_DEQUE_HALF];
            for (int32_t i = 0; i < n; i++) {
                _coro_ctx_t* c =
                    (_coro_ctx_t*)mco_get_user_data(batch[i]);
                nodes[i] = &c->runq_node;
            }
            runq_push_batch(sched->runq, nodes, n);
            _sched_wake_worker(sched);
        }
        if (wsdeque_push(_tls_worker->deque, old) == 0) {
            return;
        }
        /* Still full: last-resort global runq. */
        _coro_ctx_t* old_ctx = (_coro_ctx_t*)mco_get_user_data(old);
        runq_push(sched->runq, &old_ctx->runq_node);
        _sched_wake_worker(sched);
        return;
    }
    /* External thread: use global runq. */
    runq_push(sched->runq, &ctx->runq_node);
    _sched_wake_worker(sched);
}

void scheduler_spawn(scheduler_t* sched, void (*fn)(void*), void* arg) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)calloc(1, sizeof(_coro_ctx_t));
    if (!ctx) {
        return;
    }

    ctx->fn = fn;
    ctx->arg = arg;

    mco_desc desc = mco_desc_init(_sched_coro_entry, SCHED_CORO_STACK_SIZE);
    desc.user_data = ctx;

    mco_coro* co = NULL;
    if (mco_create(&co, &desc) != MCO_SUCCESS) {
        free(ctx);
        return;
    }

    ctx->co = co;

    spin_lock(&sched->registry_lock);
    list_insert_tail(&sched->registry, &ctx->registry_node);
    spin_unlock(&sched->registry_lock);

    atomic_fetch_add(&sched->alive, 1);
    scheduler_schedule(sched, co);
}

void scheduler_park(
    scheduler_t* sched, scheduler_park_fn_t fn, void* arg) {
    (void)sched;
    /**
     * park needs a worker TLS binding (to stash park_fn/arg) and a
     * running coroutine (for mco_yield to suspend). Catch misuse
     * here rather than UB-crash inside mco_yield.
     */
    if (!_tls_worker || !mco_running()) {
        xylem_loge(
            "scheduler_park called without a coroutine context "
            "(tls_worker=%p, mco_running=%p); park-style APIs "
            "(iowait_read/write, channel_recv, mutex_lock, "
            "waitgroup_wait, runtime_sleep/submit, tcp/udp I/O) "
            "must be called from inside a coroutine running on a "
            "scheduler worker; aborting",
            (void*)_tls_worker,
            (void*)mco_running());
        abort();
    }
    _tls_worker->park_fn  = fn;
    _tls_worker->park_arg = arg;
    mco_yield(mco_running());
}

platform_poller_sq_t* scheduler_get_poller(scheduler_t* sched) {
    return &sched->poller;
}

iowait_pool_t* scheduler_get_iowait_pool(scheduler_t* sched) {
    return sched->iowait_pool;
}

int scheduler_post(
    scheduler_t* sched, scheduler_post_fn_t cb, void* ud) {
    _sched_post_t* req = (_sched_post_t*)calloc(1, sizeof(*req));
    if (!req) {
        return -1;
    }
    req->cb = cb;
    req->ud = ud;
    mpsc_push(&sched->posts, &req->node);
    /**
     * A spinning worker will drain posts on its next blocking poll
     * without a wakeup. Only poke when everyone is idle/parked.
     */
    if (atomic_load(&sched->nspinning) == 0) {
        _sched_wake_worker(sched);
    }
    return 0;
}

sched_timer_t* sched_timer_create(scheduler_t* sched) {
    sched_timer_t* t = (sched_timer_t*)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->sched = sched;
    atomic_store_explicit(&t->refcnt, 1, memory_order_relaxed);
    return t;
}

void sched_timer_destroy(sched_timer_t* timer) {
    if (!timer) {
        return;
    }
    /**
     * stop() first so no new fires will be dequeued. A fire already
     * in flight holds its own reference, so dropping the creator
     * reference here is safe; last unref runs free().
     */
    sched_timer_stop(timer);
    _sched_timer_unref(timer);
}

void sched_timer_start(
    sched_timer_t*   timer,
    sched_timer_fn_t cb,
    void*            ud,
    uint64_t         timeout_ms,
    uint64_t         repeat_ms) {
    scheduler_t* sched = timer->sched;
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    mtx_lock(&sched->timer_lock);
    if (timer->active) {
        heap_remove(&sched->timers, &timer->heap_node);
    }
    timer->cb      = cb;
    timer->ud      = ud;
    timer->timeout = now + timeout_ms;
    timer->repeat  = repeat_ms;
    timer->active  = true;
    heap_insert(&sched->timers, &timer->heap_node);
    mtx_unlock(&sched->timer_lock);

    /**
     * A worker in blocking poll may be waiting on a stale (later)
     * heap root timeout; poke it to recompute with the new root.
     */
    _sched_wake_poller(sched);
}

bool sched_timer_stop(sched_timer_t* timer) {
    scheduler_t* sched = timer->sched;

    bool cancelled = false;
    mtx_lock(&sched->timer_lock);
    if (timer->active) {
        heap_remove(&sched->timers, &timer->heap_node);
        timer->active = false;
        cancelled = true;
    }
    mtx_unlock(&sched->timer_lock);
    return cancelled;
}

bool sched_timer_reset(sched_timer_t* timer, uint64_t timeout_ms) {
    scheduler_t* sched = timer->sched;
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    bool was_active;
    mtx_lock(&sched->timer_lock);
    was_active = timer->active;
    if (was_active) {
        heap_remove(&sched->timers, &timer->heap_node);
    }
    timer->timeout = now + timeout_ms;
    /* Periodic timers adopt timeout_ms as the new repeat interval so
     * reset() restarts the clock coherently for both one-shot and
     * periodic variants. */
    if (timer->repeat != 0) {
        timer->repeat = timeout_ms;
    }
    timer->active = true;
    heap_insert(&sched->timers, &timer->heap_node);
    mtx_unlock(&sched->timer_lock);

    /* Mirrors sched_timer_start: recompute in case blocking poll holds
     * a stale root timeout. */
    _sched_wake_poller(sched);
    return was_active;
}

void scheduler_set_idle_cb(
    scheduler_t* sched, scheduler_idle_fn_t cb, void* ud) {
    sched->idle_cb = cb;
    sched->idle_ud = ud;
}
