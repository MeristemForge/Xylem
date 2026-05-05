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

#include "sched-timer.h"
#include "wsdeque.h"
#include "runq.h"
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
#define SCHED_CORO_STACK_SIZE    131072
#define SCHED_POLL_TIMEOUT_MS    5

typedef struct _sched_worker_s {
    thrd_t               thread;
    wsdeque_t*           deque;
    platform_sem_t*      sem;
    scheduler_t*         sched;
    uint32_t             index;
    _Atomic bool         is_polling;
    scheduler_park_fn_t  park_fn;
    void*                park_arg;
} _sched_worker_t;

struct scheduler_s {
    _sched_worker_t*      workers;
    int32_t               nworkers;
    runq_t*               runq;
    sched_timer_mgr_t*    timer_mgr;
    mpsc_t                posts;
    platform_poller_sq_t* poller;
    scheduler_poll_fn_t   poll_cb;
    platform_poller_sqe_t wakeup_sqe;
    platform_sock_t       wakeup_rd;
    platform_sock_t       wakeup_wr;
    _Atomic uint32_t      notify_idx;
    _Atomic bool          polling;
    _Atomic bool          running;
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

static void _sched_coro_entry(mco_coro* co) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
    ctx->fn(ctx->arg);
}

static mco_coro* _sched_try_get_coro(scheduler_t* sched, _sched_worker_t* w) {
    mco_coro* co = wsdeque_pop(w->deque);
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
            co = wsdeque_steal(sched->workers[idx].deque);
            if (co) {
                return co;
            }
        }
    }

    return NULL;
}

static void _sched_process_poll_events(
    scheduler_t* sched,
    platform_poller_cqe_t* cqes,
    int n) {
    for (int i = 0; i < n; i++) {
        if (cqes[i].ud == NULL) {
            /* Wakeup fd -- drain and ignore. */
            char buf[64];
            while (platform_socket_recv(sched->wakeup_rd, buf, sizeof(buf)) > 0) {
            }
            /* Re-arm the wakeup fd (oneshot). */
            sched->wakeup_sqe.op = PLATFORM_POLLER_RD_OP;
            platform_poller_mod(sched->poller, &sched->wakeup_sqe);
            continue;
        }
        if (sched->poll_cb) {
            sched->poll_cb((int)cqes[i].op, cqes[i].ud);
        }
    }
}

static void _sched_process_timers_and_posts(scheduler_t* sched) {
    mpsc_node_t* node;
    while ((node = mpsc_pop(&sched->posts)) != NULL) {
        _sched_post_t* req = mpsc_entry(node, _sched_post_t, node);
        req->cb(req->ud);
        free(req);
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    sched_timer_mgr_process(sched->timer_mgr, now);
}

static int _sched_worker_entry(void* arg) {
    _sched_worker_t* w = (_sched_worker_t*)arg;
    scheduler_t* sched = w->sched;
    _tls_worker = w;

    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];

    while (atomic_load(&sched->running)) {
        mco_coro* co = _sched_try_get_coro(sched, w);

        if (co) {
            mco_resume(co);
            if (mco_status(co) == MCO_DEAD) {
                _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
                free(ctx);
                mco_destroy(co);
            } else if (w->park_fn) {
                scheduler_park_fn_t fn = w->park_fn;
                void* arg = w->park_arg;
                w->park_fn  = NULL;
                w->park_arg = NULL;
                if (!fn(co, arg)) {
                    wsdeque_push(w->deque, co);
                }
            }
            continue;
        }

        if (sched->poller) {
            bool expected = false;
            if (atomic_compare_exchange_strong(
                    &sched->polling, &expected, true)) {
                atomic_store(&w->is_polling, true);
                uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                int timer_timeout = sched_timer_mgr_next_timeout(
                    sched->timer_mgr, now);
                int poll_ms = SCHED_POLL_TIMEOUT_MS;
                if (timer_timeout >= 0 && timer_timeout < poll_ms) {
                    poll_ms = timer_timeout;
                }
                int n = platform_poller_wait(
                    sched->poller, cqes, poll_ms);
                atomic_store(&w->is_polling, false);
                if (n > 0) {
                    _sched_process_poll_events(sched, cqes, n);
                }
                _sched_process_timers_and_posts(sched);
                atomic_store(&sched->polling, false);
                continue;
            }
        }

        platform_sem_wait(w->sem);
    }

    for (;;) {
        mco_coro* co = _sched_try_get_coro(sched, w);
        if (!co) {
            break;
        }
        mco_resume(co);
        if (mco_status(co) == MCO_DEAD) {
            _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
            free(ctx);
            mco_destroy(co);
        } else if (w->park_fn) {
            scheduler_park_fn_t fn = w->park_fn;
            void* arg = w->park_arg;
            w->park_fn  = NULL;
            w->park_arg = NULL;
            if (!fn(co, arg)) {
                wsdeque_push(w->deque, co);
            }
        }
    }

    return 0;
}

static void _sched_teardown(scheduler_t* sched, int32_t nstarted) {
    atomic_store(&sched->running, false);

    if (sched->workers) {
        for (int32_t i = 0; i < nstarted; i++) {
            if (sched->workers[i].sem) {
                platform_sem_post(sched->workers[i].sem);
            }
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

    if (sched->timer_mgr) {
        sched_timer_mgr_destroy(sched->timer_mgr);
    }

    /* Drain any remaining posts. */
    {
        mpsc_node_t* node;
        while ((node = mpsc_pop(&sched->posts)) != NULL) {
            _sched_post_t* req =
                mpsc_entry(node, _sched_post_t, node);
            req->cb(req->ud);
            free(req);
        }
    }

    if (sched->wakeup_rd) {
        platform_poller_del(sched->poller, &sched->wakeup_sqe);
        platform_socket_close(sched->wakeup_rd);
        platform_socket_close(sched->wakeup_wr);
    }

    free(sched);
}

static void _sched_notify_worker(scheduler_t* sched) {
    uint32_t n = (uint32_t)sched->nworkers;
    uint32_t start =
        atomic_fetch_add(&sched->notify_idx, 1) % n;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (start + i) % n;
        if (!atomic_load(&sched->workers[idx].is_polling)) {
            platform_sem_post(sched->workers[idx].sem);
            return;
        }
    }

    /* All workers are polling or busy -- post to the original target. */
    platform_sem_post(sched->workers[start].sem);
}

static void _sched_notify_other(scheduler_t* sched, uint32_t self) {
    if (sched->nworkers <= 1) {
        return;
    }
    uint32_t n = (uint32_t)sched->nworkers;
    uint32_t idx = atomic_fetch_add(&sched->notify_idx, 1) % n;
    if (idx == self) {
        idx = (idx + 1) % n;
    }
    platform_sem_post(sched->workers[idx].sem);
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
        _sched_teardown(sched, 0);
        return NULL;
    }

    sched->timer_mgr = sched_timer_mgr_create();
    if (!sched->timer_mgr) {
        _sched_teardown(sched, 0);
        return NULL;
    }

    mpsc_init(&sched->posts);

    atomic_store(&sched->running, true);
    atomic_store(&sched->polling, false);

    sched->nworkers = nworkers;
    sched->workers = (_sched_worker_t*)calloc(
        (size_t)nworkers, sizeof(_sched_worker_t));
    if (!sched->workers) {
        _sched_teardown(sched, 0);
        return NULL;
    }

    for (int32_t i = 0; i < nworkers; i++) {
        _sched_worker_t* w = &sched->workers[i];
        w->deque = wsdeque_create(deque_log2);
        w->sem = platform_sem_create(0);
        w->sched = sched;
        w->index = (uint32_t)i;

        if (!w->deque || !w->sem) {
            _sched_teardown(sched, 0);
            return NULL;
        }
    }

    for (int32_t i = 0; i < nworkers; i++) {
        if (thrd_create(&sched->workers[i].thread,
                        _sched_worker_entry,
                        &sched->workers[i]) != thrd_success) {
            _sched_teardown(sched, i);
            return NULL;
        }
    }

    return sched;
}

void scheduler_destroy(scheduler_t* sched) {
    if (!sched) {
        return;
    }

    mco_coro* co;
    while ((co = runq_pop(sched->runq)) != NULL) {
        _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
        free(ctx);
        mco_destroy(co);
    }

    _sched_teardown(sched, sched->nworkers);
}

void scheduler_schedule(scheduler_t* sched, mco_coro* co) {
    runq_push(sched->runq, co);
    _sched_notify_worker(sched);
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

    if (_tls_worker && _tls_worker->sched == sched) {
        if (wsdeque_push(_tls_worker->deque, co) == 0) {
            _sched_notify_other(sched, _tls_worker->index);
            return;
        }
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

static void _sched_wake_poller(scheduler_t* sched) {
    if (sched->wakeup_wr) {
        char c = 1;
        platform_socket_send(sched->wakeup_wr, &c, 1);
    }
}

void scheduler_shutdown(scheduler_t* sched) {
    atomic_store(&sched->running, false);

    /* Wake the poller worker if blocked in epoll_wait. */
    if (sched->poller) {
        _sched_wake_poller(sched);
    }

    for (int32_t i = 0; i < sched->nworkers; i++) {
        platform_sem_post(sched->workers[i].sem);
    }
}

void scheduler_set_poller(
    scheduler_t* sched,
    platform_poller_sq_t* poller,
    scheduler_poll_fn_t cb) {
    sched->poller = poller;
    sched->poll_cb = cb;

    /* Create a wakeup socketpair to unblock epoll_wait on shutdown. */
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
        sched->wakeup_sqe.oneshot = 1;
        platform_poller_add(poller, &sched->wakeup_sqe);
    }
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
    _sched_notify_worker(sched);
    return 0;
}

sched_timer_mgr_t* scheduler_get_timer_mgr(scheduler_t* sched) {
    return sched->timer_mgr;
}
