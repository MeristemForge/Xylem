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
 *
 * N worker threads cooperate through a three-tier runnable pool:
 *   1. per-worker `runnext` slot   - single LIFO hand-off, cache-hot
 *   2. per-worker work-stealing    - owner pushes/pops the tail, other
 *      deque (wsdeque)               workers steal from the head
 *   3. global runq (mpsc)          - overflow from full deques, and
 *                                    injection point for cross-thread
 *                                    scheduler_schedule() callers
 *
 * When a worker runs out of local work it becomes a "searcher" and
 * does a few non-blocking poll+steal rounds. Searchers are throttled
 * to half the pool (Go/Tokio nspinning pattern) so a busy system
 * does not spend its cycles probing empty queues. The last searcher
 * out becomes the "driver": it owns the blocking poll that services
 * IO, timers and deferred posts, handing the role off as soon as
 * another worker starts searching. Non-searcher / non-driver workers
 * park on a per-worker semaphore; cross-scheduler scheduler_schedule
 * and scheduler_post wake at most one parked worker per push.
 *
 * Coroutine parking flows through scheduler_park: the park callback
 * runs *after* mco_yield returns, so a wakeup source can never
 * observe the coroutine pointer before the yield has suspended it.
 * iowait.c relies on this to avoid schedule-before-yield races.
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
#define SCHED_MAX_POLL_MS        1
#define SCHED_SPIN_ATTEMPTS      1
#define SCHED_RUNQ_GRAB_MAX      256
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
    uint32_t             sched_tick;
    uint64_t             last_poll_ns;
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
    /**
     * Set while the driver is blocked inside platform_poller_wait
     * and cleared once it returns. Producers that need to deliver a
     * non-fd wakeup (post, cross-thread schedule with no parked
     * worker) consult it to decide whether a pipe wake is actually
     * necessary: when false, the driver is already in user space
     * and will observe the new work before its next blocking poll.
     */
    _Atomic bool          driver_in_poll;
    _Atomic bool          driver_active;
    bool                  joined;
    _Atomic int32_t       nspinning;
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

#ifdef XYLEM_SCHED_STATS
#include <stdio.h>
#include <time.h>

static _Atomic uint64_t _sched_stat_poll_cycles;
static _Atomic uint64_t _sched_stat_poll_block_ns;
static _Atomic uint64_t _sched_stat_poll_events;
static _Atomic uint64_t _sched_stat_driver_nonpoll_ns;
static _Atomic uint64_t _sched_stat_driver_runs; /* # times a worker entered the driver loop */
static _Atomic uint64_t _sched_stat_wake_poller;   /* # of pipe wake calls */
static _Atomic uint64_t _sched_stat_wake_sem;      /* # of sem_post wakes */
static _Atomic uint64_t _sched_stat_wake_skipped;  /* # of schedule calls that skipped wake */

static uint64_t _sched_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void scheduler_stats_reset(void) {
    atomic_store(&_sched_stat_poll_cycles, 0);
    atomic_store(&_sched_stat_poll_block_ns, 0);
    atomic_store(&_sched_stat_poll_events, 0);
    atomic_store(&_sched_stat_driver_nonpoll_ns, 0);
    atomic_store(&_sched_stat_driver_runs, 0);
    atomic_store(&_sched_stat_wake_poller, 0);
    atomic_store(&_sched_stat_wake_sem, 0);
    atomic_store(&_sched_stat_wake_skipped, 0);
}

void scheduler_stats_dump(const char* tag) {
    uint64_t cycles  = atomic_load(&_sched_stat_poll_cycles);
    uint64_t block   = atomic_load(&_sched_stat_poll_block_ns);
    uint64_t events  = atomic_load(&_sched_stat_poll_events);
    uint64_t nonpoll = atomic_load(&_sched_stat_driver_nonpoll_ns);
    uint64_t runs    = atomic_load(&_sched_stat_driver_runs);
    uint64_t wp      = atomic_load(&_sched_stat_wake_poller);
    uint64_t ws      = atomic_load(&_sched_stat_wake_sem);
    uint64_t wk      = atomic_load(&_sched_stat_wake_skipped);
    if (cycles == 0) {
        fprintf(stderr, "[sched-stats %s] no poll cycles\n", tag);
        return;
    }
    fprintf(stderr,
            "[sched-stats %s] poll_cycles=%llu avg_block=%llu ns "
            "avg_events=%.2f nonpoll_total=%llu ms driver_runs=%llu "
            "wake_pipe=%llu wake_sem=%llu wake_skip=%llu\n",
            tag,
            (unsigned long long)cycles,
            (unsigned long long)(block / cycles),
            (double)events / (double)cycles,
            (unsigned long long)(nonpoll / 1000000ull),
            (unsigned long long)runs,
            (unsigned long long)wp,
            (unsigned long long)ws,
            (unsigned long long)wk);
}
#endif

static void _sched_wake_poller(scheduler_t* sched) {
    if (sched->wakeup_wr) {
#ifdef XYLEM_SCHED_STATS
        atomic_fetch_add_explicit(
            &_sched_stat_wake_poller, 1, memory_order_relaxed);
#endif
        char c = 1;
        platform_socket_send(sched->wakeup_wr, &c, 1);
    }
}

/**
 * Wake one parked worker; if none, only pipe-wake the driver when
 * it is actually blocked in poll. When it is in user space the
 * driver will observe the new work on its next try_get_coro /
 * process_posts pass, so the pipe wake is redundant -- and the
 * observed 17% wake_pipe volume at conn=16 was hurting throughput
 * by forcing early returns from poll with events <= 1.
 */
static void _sched_wake_worker(scheduler_t* sched) {
    for (int32_t i = 0; i < sched->nworkers; i++) {
        if (atomic_load(&sched->workers[i].parked)) {
#ifdef XYLEM_SCHED_STATS
            atomic_fetch_add_explicit(
                &_sched_stat_wake_sem, 1, memory_order_relaxed);
#endif
            platform_sem_post(sched->workers[i].sem);
            return;
        }
    }
    if (atomic_load_explicit(
            &sched->driver_in_poll, memory_order_seq_cst)) {
        _sched_wake_poller(sched);
    }
}

static mco_coro* _sched_try_get_coro(scheduler_t* sched, _sched_worker_t* w) {
    mco_coro* co = atomic_exchange(&w->runnext, NULL);
    if (co) {
        return co;
    }

    co = wsdeque_pop(w->deque);
    if (co) {
        return co;
    }

    {
        /**
         * Fair share from global runq: grab up to half of the
         * currently-queued work (capped) so a searcher refills its
         * local deque in one hop instead of coming back round after
         * round. Mirrors Go's runqgrab.
         */
        queue_node_t* nodes[SCHED_RUNQ_GRAB_MAX];
        int32_t n = runq_pop_half(
            sched->runq, nodes, SCHED_RUNQ_GRAB_MAX);
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

/* ms until the earliest timer fires, or -1 if none. Caller holds timer_lock. */
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

/**
 * Process poll events. Returns the first ready coroutine directly
 * (Go's gp := list.pop() pattern — zero global-runq round-trip).
 * Remaining coroutines are injected into the global runq.
 */
static mco_coro* _sched_process_io(
    scheduler_t* sched,
    platform_poller_cqe_t* cqes,
    int n) {
    mco_coro* batch_buf[PLATFORM_POLLER_CQE_NUM * 2];
    runnable_batch_t batch = {
        .buf = batch_buf,
        .cap = (int32_t)(sizeof(batch_buf) / sizeof(batch_buf[0])),
        .n   = 0,
    };

    for (int i = 0; i < n; i++) {
        if (cqes[i].ud == NULL) {
            char buf[64];
            while (platform_socket_recv(sched->wakeup_rd, buf, sizeof(buf)) > 0) {
            }
            if (PLATFORM_POLLER_TRIGGER_MODE != PLATFORM_POLLER_TRIGGER_ET) {
                sched->wakeup_sqe.op = PLATFORM_POLLER_RD_OP;
                platform_poller_mod(&sched->poller, &sched->wakeup_sqe);
            }
            continue;
        }
        iowait_on_event(sched, (int)cqes[i].op, cqes[i].ud, &batch);
    }

    if (batch.n <= 0) {
        return NULL;
    }

    mco_coro* first = batch.buf[0];
    if (batch.n > 1) {
        scheduler_schedule_batch(sched, &batch.buf[1], batch.n - 1);
    }
    return first;
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
            /**
             * Park declined: reschedule. Fall back to global runq on
             * local overflow so we never drop a live coroutine.
             */
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
 * Opportunistic timer service on the worker fast path.
 *
 * A CPU-bound workload keeps every worker busy on runnable coroutines
 * and nobody reaches the driver's blocking poll, so timers would
 * starve without this tick. One CAS on timer_last_tick_ms elects a
 * single worker per window; everyone else pays just an atomic load.
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

/**
 * Find a runnable coroutine, or return NULL to tell the caller to park.
 *
 * Implements the Go/Tokio nspinning pattern: at most half the workers
 * are allowed to be searching at once, and the last searcher out of
 * the spin phase becomes the driver that runs the blocking-poll loop
 * (IO / timers / posts). The driver hands off as soon as another
 * worker starts searching, so blocking-poll duty is never pinned.
 */
/**
 * Go findRunnable() equivalent. Steps numbered to match Go's proc.go.
 *
 * Go:  local → global(size/nprocs+1) → netpoll(0) → steal → spin/block
 * Ours: same order, same grab formula, same netpoll placement.
 */
static mco_coro* _sched_find_work(
    scheduler_t*           sched,
    _sched_worker_t*       w,
    platform_poller_cqe_t* cqes) {

    /* Go step 1: local runq (runnext + deque). */
    mco_coro* co = atomic_exchange(&w->runnext, NULL);
    if (co) {
        return co;
    }
    co = wsdeque_pop(w->deque);
    if (co) {
        return co;
    }

    /* Go step 2: global runq — grab size/nworkers+1 (Go's globrunqget). */
    {
        queue_node_t* nodes[SCHED_RUNQ_GRAB_MAX];
        int32_t n = runq_pop_nprocs(
            sched->runq, nodes, SCHED_RUNQ_GRAB_MAX,
            sched->nworkers);
        if (n > 0) {
            for (int32_t i = 1; i < n; i++) {
                _coro_ctx_t* c =
                    queue_entry(nodes[i], _coro_ctx_t, runq_node);
                wsdeque_push(w->deque, c->co);
            }
            _coro_ctx_t* ctx =
                queue_entry(nodes[0], _coro_ctx_t, runq_node);
            return ctx->co;
        }
    }

    /* Go step 3: netpoll(0) — non-blocking poll before steal.
     * Skip when a driver is already blocking in epoll_wait: our
     * poll would just race it for the same events. Skipping lets
     * the worker reach the park path sooner (more futex wakes,
     * fewer redundant epoll_wait calls). */
    if (!atomic_load_explicit(
            &sched->driver_active, memory_order_relaxed)) {
        int n = platform_poller_wait(&sched->poller, cqes, 0);
        if (n > 0) {
            co = _sched_process_io(sched, cqes, n);
            if (co) {
                return co;
            }
        }
    }

    /* Go step 4: steal from other workers. */
    if (sched->nworkers > 1) {
        uint32_t start = w->index + 1;
        for (int32_t i = 0; i < sched->nworkers - 1; i++) {
            uint32_t idx =
                (start + (uint32_t)i) % (uint32_t)sched->nworkers;
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

    /**
     * Go step 5: become the blocking-poll driver, or park.
     *
     * No spin phase: step 3 already did a non-blocking poll, and
     * spinning again within microseconds finds nothing new but costs
     * a syscall. Instead, exactly one worker becomes the driver via
     * CAS; all others park on their semaphore and await a direct
     * futex wake (matching Go's stopm/wakep pattern).
     */
    {
        bool expected = false;
        if (!atomic_compare_exchange_strong_explicit(
                &sched->driver_active,
                &expected,
                true,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return NULL;
        }
    }

#ifdef XYLEM_SCHED_STATS
    atomic_fetch_add_explicit(
        &_sched_stat_driver_runs, 1, memory_order_relaxed);
#endif

    while (atomic_load(&sched->running)) {
        int poll_ms = _sched_timer_next_timeout(sched);
        if (poll_ms < 0 || poll_ms > SCHED_MAX_POLL_MS) {
            poll_ms = SCHED_MAX_POLL_MS;
        }
#ifdef XYLEM_SCHED_STATS
        uint64_t t0 = _sched_now_ns();
#endif
        atomic_store_explicit(
            &sched->driver_in_poll, true, memory_order_seq_cst);
        co = _sched_try_get_coro(sched, w);
        if (co) {
            atomic_store_explicit(
                &sched->driver_in_poll, false, memory_order_relaxed);
            atomic_store_explicit(
                &sched->driver_active, false, memory_order_release);
            return co;
        }
        int n = platform_poller_wait(&sched->poller, cqes, poll_ms);
        atomic_store_explicit(
            &sched->driver_in_poll, false, memory_order_relaxed);
#ifdef XYLEM_SCHED_STATS
        uint64_t t1 = _sched_now_ns();
        atomic_fetch_add_explicit(
            &_sched_stat_poll_cycles, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(
            &_sched_stat_poll_block_ns, t1 - t0, memory_order_relaxed);
        if (n > 0) {
            atomic_fetch_add_explicit(
                &_sched_stat_poll_events,
                (uint64_t)n,
                memory_order_relaxed);
        }
#endif
        if (n > 0) {
            mco_coro* first = _sched_process_io(sched, cqes, n);
            for (;;) {
                int extra = platform_poller_wait(&sched->poller, cqes, 0);
                if (extra <= 0) {
                    break;
                }
#ifdef XYLEM_SCHED_STATS
                atomic_fetch_add_explicit(
                    &_sched_stat_poll_cycles, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(
                    &_sched_stat_poll_events,
                    (uint64_t)extra,
                    memory_order_relaxed);
#endif
                mco_coro* extra_co = _sched_process_io(sched, cqes, extra);
                if (extra_co) {
                    if (!first) {
                        first = extra_co;
                    } else {
                        scheduler_schedule(sched, extra_co);
                    }
                }
            }
            if (first) {
                co = first;
            }
        }

        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        _sched_process_timers(sched, now);

        /**
         * mpsc_pop is single-consumer; CAS elects exactly one drainer
         * at a time. Losers skip this pass -- the winner (or our next
         * round after it releases) will pick the work up.
         */
        bool expected = false;
        if (atomic_compare_exchange_strong(
                &sched->processing, &expected, true)) {
            _sched_process_posts(sched);
            atomic_store(&sched->processing, false);
        }

        co = _sched_try_get_coro(sched, w);
#ifdef XYLEM_SCHED_STATS
        {
            uint64_t t2 = _sched_now_ns();
            atomic_fetch_add_explicit(
                &_sched_stat_driver_nonpoll_ns,
                t2 - t1,
                memory_order_relaxed);
        }
#endif
        if (co) {
            /**
             * Run one coroutine inline and loop back to poll.
             * The driver never exits — this eliminates the
             * driver-handoff gap that causes p50 spikes. One
             * worker (6.25% of 16) acts as a combined poller
             * + executor, matching Go's tight poll/run loop.
             */
            atomic_store_explicit(
                &sched->driver_in_poll, false, memory_order_relaxed);
            _sched_run_coro(w, co);
            _sched_timer_tick(sched);
        }
    }
    atomic_store_explicit(
        &sched->driver_active, false, memory_order_release);
    return NULL;
}

static int _sched_worker_entry(void* arg) {
    _sched_worker_t* w = (_sched_worker_t*)arg;
    scheduler_t* sched = w->sched;
    _tls_worker = w;

    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];

    while (atomic_load(&sched->running)) {
        _sched_timer_tick(sched);

        /**
         * Go schedule(): every 61st execution, grab one from global
         * runq before checking local, ensuring fairness.
         */
        mco_coro* co = NULL;
        if (++w->sched_tick % 61 == 0) {
            queue_node_t* node = runq_pop(sched->runq);
            if (node) {
                co = queue_entry(node, _coro_ctx_t, runq_node)->co;
            }
        }
        if (!co) {
            co = _sched_find_work(sched, w, cqes);
        }
        if (co) {
            _sched_run_coro(w, co);
            continue;
        }

        atomic_store(&w->parked, true);
        platform_sem_wait(w->sem);
        atomic_store(&w->parked, false);
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

    /* External thread or cross-scheduler: straight to global runq. */
    if (!_tls_worker || _tls_worker->sched != sched) {
        runq_push(sched->runq, &ctx->runq_node);
    } else {
        mco_coro* old = atomic_exchange(&_tls_worker->runnext, co);
        if (old && wsdeque_push(_tls_worker->deque, old) != 0) {
            /**
             * Deque full: spill the front half plus the displaced task
             * into the global runq in one pass. Mirrors Go's
             * runqputslow and Tokio's push_overflow -- a single
             * batched push is simpler than splitting and racing
             * other stealers.
             */
            mco_coro* batch[SCHED_DEQUE_HALF + 1];
            int32_t n = wsdeque_pop_half(
                _tls_worker->deque, batch, SCHED_DEQUE_HALF);
            batch[n++] = old;

            queue_node_t* nodes[SCHED_DEQUE_HALF + 1];
            for (int32_t i = 0; i < n; i++) {
                _coro_ctx_t* c =
                    (_coro_ctx_t*)mco_get_user_data(batch[i]);
                nodes[i] = &c->runq_node;
            }
            runq_push_batch(sched->runq, nodes, n);
        }
    }

    /**
     * Ensure at least one worker is searching so the new task gets
     * picked up promptly. Throttled on nspinning -- mirrors Go
     * wakep() and Tokio notify_parked: if a searcher already exists
     * it will discover the task via steal / runq grab on its next
     * round, so waking another worker would only contend on CAS.
     * When the searcher takes a task and starts running, nspinning
     * drops back to 0; the next push wakes the next worker, so
     * parallelism expands one step at a time until load is spread
     * across cores. Without this, the local-deque fast path above
     * can leave newly scheduled work invisible to parked workers
     * until the deque overflows (up to 1024 tasks).
     */
    if (atomic_load_explicit(
            &sched->nspinning, memory_order_relaxed) == 0) {
        _sched_wake_worker(sched);
    } else {
#ifdef XYLEM_SCHED_STATS
        atomic_fetch_add_explicit(
            &_sched_stat_wake_skipped, 1, memory_order_relaxed);
#endif
    }
}

void scheduler_schedule_batch(
    scheduler_t* sched, mco_coro** cos, int32_t n) {
    if (n <= 0) {
        return;
    }

    /**
     * Straight to global runq: every worker reaches this via
     * _sched_try_get_coro's runq_pop_batch, spreading the load in
     * one hop instead of going through any single worker's local
     * deque. This is the Go injectglist / Tokio inject-on-park-ready
     * model and is the main reason netpoll-produced work does not
     * pile up on the driver alone.
     *
     * Up to 64 inline; larger batches allocate. Typical netpoll
     * batches are well below 64 even on busy 10k-conn workloads.
     */
    enum { INLINE_CAP = 64 };
    queue_node_t*  inline_nodes[INLINE_CAP];
    queue_node_t** nodes = inline_nodes;
    if (n > INLINE_CAP) {
        nodes = (queue_node_t**)malloc((size_t)n * sizeof(*nodes));
        if (!nodes) {
            /* Fall back to per-coro schedule; slower but never drops. */
            for (int32_t i = 0; i < n; i++) {
                scheduler_schedule(sched, cos[i]);
            }
            return;
        }
    }
    for (int32_t i = 0; i < n; i++) {
        _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(cos[i]);
        nodes[i] = &ctx->runq_node;
    }
    runq_push_batch(sched->runq, nodes, n);
    if (nodes != inline_nodes) {
        free(nodes);
    }

    if (atomic_load_explicit(
            &sched->nspinning, memory_order_relaxed) == 0) {
        _sched_wake_worker(sched);
    } else {
#ifdef XYLEM_SCHED_STATS
        atomic_fetch_add_explicit(
            &_sched_stat_wake_skipped, 1, memory_order_relaxed);
#endif
    }
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
     * without a wakeup. Only poke when everyone is idle/parked;
     * _sched_wake_worker's pipe fall-through is driver_in_poll
     * gated so a busy driver is not spuriously woken.
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
     * stop() so no new fires are dequeued. A fire already in flight
     * holds its own ref, so dropping the creator ref is safe; the
     * last unref runs free().
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
    /**
     * Periodic timers adopt timeout_ms as the new repeat interval so
     * reset() restarts the clock coherently for both one-shot and
     * periodic variants.
     */
    if (timer->repeat != 0) {
        timer->repeat = timeout_ms;
    }
    timer->active = true;
    heap_insert(&sched->timers, &timer->heap_node);
    mtx_unlock(&sched->timer_lock);

    /**
     * Mirrors sched_timer_start: recompute in case blocking poll
     * holds a stale root timeout.
     */
    _sched_wake_poller(sched);
    return was_active;
}

void scheduler_set_idle_cb(
    scheduler_t* sched, scheduler_idle_fn_t cb, void* ud) {
    sched->idle_cb = cb;
    sched->idle_ud = ud;
}
