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

#include "scheduler.h"

#include "xylem/xylem-utils.h"

#include "iowait.h"
#include "wsdeque.h"
#include "runq.h"
#include "container/heap.h"
#include "container/mpsc.h"
#include "platform/platform-sem.h"
#include "platform/platform-socket.h"
#include "platform/platform-info.h"
#include "c11-threads.h"

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
    _Atomic bool          processing;
    _Atomic bool          running;
    _Atomic int32_t       nspinning;
    _Atomic int32_t       nparked;
};

static thread_local _sched_worker_t* _tls_worker;

typedef struct {
    void (*fn)(void*);
    void* arg;
} _coro_ctx_t;

typedef struct {
    mpsc_node_t          node;
    scheduler_post_fn_t  cb;
    void*                ud;
} _sched_post_t;

struct sched_timer_s {
    heap_node_t      heap_node;
    scheduler_t*     sched;
    sched_timer_fn_t cb;
    void*            ud;
    uint64_t         timeout;
    uint64_t         repeat;
    bool             active;
};

static void _sched_coro_entry(mco_coro* co) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
    ctx->fn(ctx->arg);
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

/* Wake one parked worker via its semaphore. */
static void _sched_wake_worker(scheduler_t* sched) {
    for (int32_t i = 0; i < sched->nworkers; i++) {
        if (atomic_load(&sched->workers[i].parked)) {
            platform_sem_post(sched->workers[i].sem);
            return;
        }
    }
    /* No parked worker found -- wake the poller to unblock epoll_wait. */
    _sched_wake_poller(sched);
}

static mco_coro* _sched_try_get_coro(scheduler_t* sched, _sched_worker_t* w) {
    /* Highest priority: runnext slot (LIFO, cache-hot). */
    mco_coro* co = atomic_exchange(&w->runnext, NULL);
    if (co) {
        return co;
    }

    co = wsdeque_pop(w->deque);
    if (co) {
        return co;
    }

    co = runq_pop(sched->runq);
    if (co) {
        return co;
    }

    if (sched->nworkers > 1) {
        uint32_t start = w->index + 1;
        for (int32_t i = 0; i < sched->nworkers - 1; i++) {
            uint32_t idx = (start + (uint32_t)i) % (uint32_t)sched->nworkers;
            mco_coro* batch[SCHED_DEQUE_HALF];
            int32_t n = wsdeque_steal_half(
                sched->workers[idx].deque, batch, SCHED_DEQUE_HALF);
            if (n > 0) {
                /* Push all but the first into our local deque. */
                for (int32_t j = 1; j < n; j++) {
                    wsdeque_push(w->deque, batch[j]);
                }
                return batch[0];
            }
        }
    }

    return NULL;
}

static int _sched_timer_next_timeout(scheduler_t* sched) {
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    mtx_lock(&sched->timer_lock);
    heap_node_t* root = heap_peek(&sched->timers);
    if (!root) {
        mtx_unlock(&sched->timer_lock);
        return -1;
    }
    sched_timer_t* t = heap_entry(root, sched_timer_t, heap_node);
    int timeout;
    if (t->timeout <= now) {
        timeout = 0;
    } else {
        uint64_t diff = t->timeout - now;
        timeout = (diff > INT32_MAX) ? INT32_MAX : (int)diff;
    }
    mtx_unlock(&sched->timer_lock);
    return timeout;
}

static int _sched_process_timers(scheduler_t* sched, uint64_t now_ms) {
    for (;;) {
        sched_timer_t* timer = NULL;

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
                timer = t;
            }
        }
        mtx_unlock(&sched->timer_lock);

        if (!timer) {
            break;
        }
        timer->cb(timer, timer->ud);
    }

    /* Return ms until next timer, or -1 if none. */
    mtx_lock(&sched->timer_lock);
    heap_node_t* root = heap_peek(&sched->timers);
    if (!root) {
        mtx_unlock(&sched->timer_lock);
        return -1;
    }
    sched_timer_t* t = heap_entry(root, sched_timer_t, heap_node);
    int timeout;
    if (t->timeout <= now_ms) {
        timeout = 0;
    } else {
        uint64_t diff = t->timeout - now_ms;
        timeout = (diff > INT32_MAX) ? INT32_MAX : (int)diff;
    }
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

static void _sched_process_events(
    scheduler_t* sched,
    platform_poller_cqe_t* cqes,
    int n) {
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
        iowait_on_event((int)cqes[i].op, cqes[i].ud);
    }
}

static void _sched_handle_yield(_sched_worker_t* w, mco_coro* co) {
    if (mco_status(co) == MCO_DEAD) {
        _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
        free(ctx);
        mco_destroy(co);
        return;
    }
    if (w->park_fn) {
        scheduler_park_fn_t fn = w->park_fn;
        void* arg = w->park_arg;
        w->park_fn  = NULL;
        w->park_arg = NULL;
        if (!fn(co, arg)) {
            wsdeque_push(w->deque, co);
        }
    }
}

static inline void _sched_run_coro(_sched_worker_t* w, mco_coro* co) {
    mco_resume(co);
    _sched_handle_yield(w, co);
}

static int _sched_worker_entry(void* arg) {
    _sched_worker_t* w = (_sched_worker_t*)arg;
    scheduler_t* sched = w->sched;
    _tls_worker = w;

    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];

    while (atomic_load(&sched->running)) {
        /* Fast path: try to get a coroutine to run. */
        mco_coro* co = _sched_try_get_coro(sched, w);

        if (co) {
            _sched_run_coro(w, co);
            continue;
        }

        /* No work found -- enter spinning state. */
        atomic_fetch_add(&sched->nspinning, 1);

        bool found_work = false;
        for (int spin = 0; spin < SCHED_SPIN_ATTEMPTS; spin++) {
            /* Non-blocking poll: each spinner grabs its own IO events. */
            int n = platform_poller_wait(&sched->poller, cqes, 0);
            if (n > 0) {
                _sched_process_events(sched, cqes, n);
            }

            /* Try to get a coroutine (may have been woken by the poll). */
            co = _sched_try_get_coro(sched, w);
            if (co) {
                found_work = true;
                break;
            }
        }

        if (found_work) {
            atomic_fetch_sub(&sched->nspinning, 1);
            _sched_run_coro(w, co);
            continue;
        }

        /* Spin failed. If we are the last spinner, do a blocking poll
         * before parking so IO events are not missed. */
        int32_t prev = atomic_fetch_sub(&sched->nspinning, 1);
        if (prev == 1) {
            /* Last spinner: do a blocking poll instead of parking.
             * We must keep polling as long as no other worker is spinning
             * to ensure IO events are always serviced. */
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
                    _sched_process_events(sched, cqes, n);
                }

                /* Process timers and posts. */
                uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                _sched_process_timers(sched, now);

                bool expected = false;
                if (atomic_compare_exchange_strong(
                        &sched->processing, &expected, true)) {
                    _sched_process_posts(sched);
                    atomic_store(&sched->processing, false);
                }

                /* Check for work -- if found, break out to execute it. */
                co = _sched_try_get_coro(sched, w);
                if (co) {
                    break;
                }

                /* If another worker started spinning, we can park. */
                if (atomic_load(&sched->nspinning) > 0) {
                    break;
                }
            }

            if (co) {
                _sched_run_coro(w, co);
            }
            continue;
        }

        /* Not the last spinner: park and wait for wakeup. */
        atomic_store(&w->parked, true);
        atomic_fetch_add(&sched->nparked, 1);
        platform_sem_wait(w->sem);
        atomic_store(&w->parked, false);
        atomic_fetch_sub(&sched->nparked, 1);
    }

    /* Drain remaining work after shutdown. */
    for (;;) {
        mco_coro* co = _sched_try_get_coro(sched, w);
        if (!co) {
            break;
        }
        _sched_run_coro(w, co);
    }

    return 0;
}

static void _sched_cleanup(scheduler_t* sched, int32_t nstarted) {
    atomic_store(&sched->running, false);

    if (sched->workers) {
        /* Wake all workers: both parked (sem) and polling (wakeup pipe). */
        for (int32_t i = 0; i < nstarted; i++) {
            platform_sem_post(sched->workers[i].sem);
            _sched_wake_poller(sched);
        }
        for (int32_t i = 0; i < nstarted; i++) {
            thrd_join(sched->workers[i].thread, NULL);
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

    mtx_destroy(&sched->timer_lock);

    _sched_process_posts(sched);

    if (sched->wakeup_rd) {
        platform_poller_del(&sched->poller, &sched->wakeup_sqe);
        platform_socket_close(sched->wakeup_rd);
        platform_socket_close(sched->wakeup_wr);
    }

    platform_poller_destroy(&sched->poller);
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

    atomic_store(&sched->running, false);
    for (int32_t i = 0; i < sched->nworkers; i++) {
        platform_sem_post(sched->workers[i].sem);
        _sched_wake_poller(sched);
    }

    mco_coro* co;
    while ((co = runq_pop(sched->runq)) != NULL) {
        _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
        free(ctx);
        mco_destroy(co);
    }

    _sched_cleanup(sched, sched->nworkers);
}

void scheduler_schedule(scheduler_t* sched, mco_coro* co) {
    /* Fast path: if called from a worker thread, use runnext slot. */
    if (_tls_worker && _tls_worker->sched == sched) {
        mco_coro* old = atomic_exchange(&_tls_worker->runnext, co);
        if (!old) {
            return;
        }
        /* Slot was occupied -- push the old one to local deque. */
        if (wsdeque_push(_tls_worker->deque, old) == 0) {
            return;
        }
        /* Local deque full: drain half to global runq, then retry. */
        mco_coro* batch[SCHED_DEQUE_HALF];
        int32_t n = wsdeque_pop_half(_tls_worker->deque, batch, SCHED_DEQUE_HALF);
        if (n > 0) {
            runq_push_batch(sched->runq, batch, n);
            _sched_wake_worker(sched);
        }
        if (wsdeque_push(_tls_worker->deque, old) == 0) {
            return;
        }
        /* Still full -- fallback to global runq. */
        runq_push(sched->runq, old);
        _sched_wake_worker(sched);
        return;
    }
    /* Slow path: external thread -- use global runq. */
    runq_push(sched->runq, co);
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

    scheduler_schedule(sched, co);
}

void scheduler_park(
    scheduler_t* sched, scheduler_park_fn_t fn, void* arg) {
    (void)sched;
    _tls_worker->park_fn  = fn;
    _tls_worker->park_arg = arg;
    mco_yield(mco_running());
}

platform_poller_sq_t* scheduler_get_poller(scheduler_t* sched) {
    return &sched->poller;
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
    return t;
}

void sched_timer_destroy(sched_timer_t* timer) {
    if (!timer) {
        return;
    }
    if (timer->active) {
        sched_timer_stop(timer);
    }
    free(timer);
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

    _sched_wake_poller(sched);
}

void sched_timer_stop(sched_timer_t* timer) {
    if (!timer->active) {
        return;
    }
    scheduler_t* sched = timer->sched;

    mtx_lock(&sched->timer_lock);
    if (timer->active) {
        heap_remove(&sched->timers, &timer->heap_node);
        timer->active = false;
    }
    mtx_unlock(&sched->timer_lock);
}
