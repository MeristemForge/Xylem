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

#include "wsdeque.h"
#include "runq.h"
#include "platform/platform-sem.h"
#include "platform/platform-info.h"
#include "c11-threads.h"

#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#define SCHED_DEFAULT_DEQUE_LOG2 10
#define SCHED_CORO_STACK_SIZE    131072

typedef struct _sched_worker_s {
    thrd_t          thread;
    wsdeque_t*      deque;
    platform_sem_t* sem;
    scheduler_t*    sched;
    uint32_t        index;
} _sched_worker_t;

struct scheduler_s {
    _sched_worker_t* workers;
    int32_t          nworkers;
    runq_t*          runq;
    _Atomic uint32_t notify_idx;
    _Atomic bool     running;
};

static thread_local _sched_worker_t* _tls_worker;

typedef struct {
    void (*fn)(void*);
    void* arg;
} _coro_ctx_t;

static void _sched_coro_entry(mco_coro* co) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
    ctx->fn(ctx->arg);
}

static mco_coro* _sched_try_get_coro(scheduler_t* sched, _sched_worker_t* w) {
    /* 1. Local deque (LIFO). */
    mco_coro* co = wsdeque_pop(w->deque);
    if (co) {
        return co;
    }

    /* 2. Global run queue. */
    co = runq_pop(sched->runq);
    if (co) {
        return co;
    }

    /* 3. Steal from random peer. */
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

static int _sched_worker_entry(void* arg) {
    _sched_worker_t* w = (_sched_worker_t*)arg;
    scheduler_t* sched = w->sched;
    _tls_worker = w;

    while (atomic_load(&sched->running)) {
        mco_coro* co = _sched_try_get_coro(sched, w);

        if (!co) {
            platform_sem_wait(w->sem);
            continue;
        }

        mco_resume(co);

        if (mco_status(co) == MCO_DEAD) {
            _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
            free(ctx);
            mco_destroy(co);
        }
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
        }
    }

    return 0;
}

/**
 * @brief Tear down a partially or fully initialised scheduler.
 *
 * Safe to call at any stage of construction: NULL pointers and
 * zero-initialised fields (from calloc) are silently skipped.
 *
 * @param sched       Scheduler to tear down (must not be NULL).
 * @param nstarted    Number of worker threads that were successfully started.
 */
static void _sched_teardown(scheduler_t* sched, int32_t nstarted) {
    atomic_store(&sched->running, false);

    if (sched->workers) {
        /* Wake threads that are alive so they can exit. */
        for (int32_t i = 0; i < nstarted; i++) {
            if (sched->workers[i].sem) {
                platform_sem_post(sched->workers[i].sem);
            }
        }
        /* Join only the threads we actually created. */
        for (int32_t i = 0; i < nstarted; i++) {
            thrd_join(sched->workers[i].thread, NULL);
        }
        /* Release per-worker resources. */
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
    free(sched);
}

static void _sched_notify_worker(scheduler_t* sched) {
    uint32_t idx =
        atomic_fetch_add(&sched->notify_idx, 1) % (uint32_t)sched->nworkers;
    platform_sem_post(sched->workers[idx].sem);
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
    atomic_store(&sched->running, true);

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

    /* Drain pending coroutines before tearing down. */
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

    /* Otherwise (or if deque full), go through global run queue. */
    scheduler_schedule(sched, co);
}

void scheduler_shutdown(scheduler_t* sched) {
    atomic_store(&sched->running, false);
    for (int32_t i = 0; i < sched->nworkers; i++) {
        platform_sem_post(sched->workers[i].sem);
    }
}
