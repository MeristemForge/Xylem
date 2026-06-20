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

#include "dynpool.h"

#include "container/mpsc.h"
#include "platform/platform-sem.h"
#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#define DYNPOOL_DEFAULT_MAX_THREADS  512
#define DYNPOOL_DEFAULT_IDLE_TIMEOUT 10000

typedef struct _dynpool_job_s {
    void (*routine)(void*);
    void*       arg;
    mpsc_node_t node;
} _dynpool_job_t;

struct dynpool_s {
    mpsc_t           queue;
    mtx_t            pop_mtx;
    platform_sem_t*  sem;
    _Atomic int32_t  thread_count;
    _Atomic int32_t  idle_count;
    int32_t          max_threads;
    uint64_t         idle_timeout;
    _Atomic bool     running;
};

static _dynpool_job_t* _dynpool_pop_job(dynpool_t* pool) {
    mtx_lock(&pool->pop_mtx);
    mpsc_node_t* node = mpsc_pop(&pool->queue);
    mtx_unlock(&pool->pop_mtx);

    if (node) {
        return mpsc_entry(node, _dynpool_job_t, node);
    }
    return NULL;
}

static int _dynpool_thread_entry(void* arg) {
    dynpool_t* pool = (dynpool_t*)arg;

    for (;;) {
        atomic_fetch_add(&pool->idle_count, 1);
        int rc = platform_sem_timedwait(pool->sem, pool->idle_timeout);
        atomic_fetch_sub(&pool->idle_count, 1);

        if (!atomic_load(&pool->running)) {
            break;
        }

        _dynpool_job_t* job = _dynpool_pop_job(pool);
        if (job) {
            job->routine(job->arg);
            free(job);
        } else if (rc != 0) {
            break;
        }
    }

    atomic_fetch_sub(&pool->thread_count, 1);
    return 0;
}

static bool _dynpool_reserve_thread(dynpool_t* pool) {
    int32_t count = atomic_load(&pool->thread_count);
    while (count < pool->max_threads) {
        if (atomic_compare_exchange_weak(
                &pool->thread_count, &count, count + 1)) {
            return true;
        }
    }
    return false;
}

static void _dynpool_spawn_thread(dynpool_t* pool) {
    if (!_dynpool_reserve_thread(pool)) {
        return;
    }

    thrd_t thr;
    if (thrd_create(&thr, _dynpool_thread_entry, pool) == thrd_success) {
        thrd_detach(thr);
    } else {
        atomic_fetch_sub(&pool->thread_count, 1);
    }
}

dynpool_t* dynpool_create(dynpool_opts_t* opts) {
    dynpool_t* pool = (dynpool_t*)calloc(1, sizeof(dynpool_t));
    if (!pool) {
        return NULL;
    }

    mpsc_init(&pool->queue);
    if (mtx_init(&pool->pop_mtx, mtx_plain) != thrd_success) {
        free(pool);
        return NULL;
    }

    pool->sem = platform_sem_create(0);
    if (!pool->sem) {
        mtx_destroy(&pool->pop_mtx);
        free(pool);
        return NULL;
    }

    pool->max_threads = DYNPOOL_DEFAULT_MAX_THREADS;
    pool->idle_timeout = DYNPOOL_DEFAULT_IDLE_TIMEOUT;

    if (opts) {
        if (opts->max_threads > 0) {
            pool->max_threads = opts->max_threads;
        }
        if (opts->idle_timeout > 0) {
            pool->idle_timeout = opts->idle_timeout;
        }
    }

    atomic_store(&pool->running, true);
    return pool;
}

int dynpool_submit(dynpool_t* pool, void (*routine)(void*), void* arg) {
    if (!pool || !routine || !atomic_load(&pool->running)) {
        return -1;
    }

    _dynpool_job_t* job = (_dynpool_job_t*)calloc(1, sizeof(_dynpool_job_t));
    if (!job) {
        return -1;
    }

    job->routine = routine;
    job->arg = arg;

    mpsc_push(&pool->queue, &job->node);
    platform_sem_post(pool->sem);

    if (atomic_load(&pool->idle_count) == 0) {
        _dynpool_spawn_thread(pool);
    }

    return 0;
}

void dynpool_destroy(dynpool_t* pool) {
    if (!pool) {
        return;
    }

    atomic_store(&pool->running, false);

    int32_t count = atomic_load(&pool->thread_count);
    for (int32_t i = 0; i < count; i++) {
        platform_sem_post(pool->sem);
    }

    while (atomic_load(&pool->thread_count) > 0) {
        thrd_yield();
    }

    mpsc_node_t* node;
    while ((node = mpsc_pop(&pool->queue)) != NULL) {
        _dynpool_job_t* job = mpsc_entry(node, _dynpool_job_t, node);
        free(job);
    }

    platform_sem_destroy(pool->sem);
    mtx_destroy(&pool->pop_mtx);
    free(pool);
}
