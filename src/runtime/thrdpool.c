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

#include "runtime/thrdpool.h"

#include "container/mpsc.h"
#include "platform/platform-sem.h"
#include "c11-threads.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct _thrdpool_job_s _thrdpool_job_t;

struct _thrdpool_job_s {
    void (*routine)(void*);
    void*       arg;
    mpsc_node_t node;
};

typedef struct _thrdpool_worker_s {
    thrd_t          thread;
    mpsc_t          queue;
    platform_sem_t* sem;
    thrdpool_t*     pool;
} _thrdpool_worker_t;

struct thrdpool_s {
    _thrdpool_worker_t* workers;
    int32_t             nworkers;
    _Atomic uint32_t    next;
    _Atomic bool        running;
};

static int _thrdpool_worker_entry(void* arg) {
    _thrdpool_worker_t* w = (_thrdpool_worker_t*)arg;

    while (atomic_load(&w->pool->running)) {
        platform_sem_wait(w->sem);

        mpsc_node_t* node = mpsc_pop(&w->queue);
        if (node) {
            _thrdpool_job_t* job = mpsc_entry(node, _thrdpool_job_t, node);
            job->routine(job->arg);
            free(job);
        }
    }

    /* Drain remaining jobs on shutdown. */
    mpsc_node_t* node;
    while ((node = mpsc_pop(&w->queue)) != NULL) {
        _thrdpool_job_t* job = mpsc_entry(node, _thrdpool_job_t, node);
        job->routine(job->arg);
        free(job);
    }
    return 0;
}

thrdpool_t* thrdpool_create(int nthrds) {
    thrdpool_t* pool = (thrdpool_t*)calloc(1, sizeof(thrdpool_t));
    if (!pool) {
        return NULL;
    }

    atomic_store(&pool->running, true);
    atomic_store(&pool->next, 0);
    pool->nworkers = 0;
    pool->workers = (_thrdpool_worker_t*)calloc(
        (size_t)nthrds, sizeof(_thrdpool_worker_t));
    if (!pool->workers) {
        free(pool);
        return NULL;
    }

    for (int i = 0; i < nthrds; i++) {
        _thrdpool_worker_t* w = &pool->workers[i];
        mpsc_init(&w->queue);
        w->sem = platform_sem_create(0);
        if (!w->sem) {
            continue;
        }
        w->pool = pool;
        if (thrd_create(&w->thread, _thrdpool_worker_entry, w) ==
            thrd_success) {
            pool->nworkers++;
        }
    }
    return pool;
}

int thrdpool_submit(
    thrdpool_t* restrict pool, void (*routine)(void*), void* arg) {
    _thrdpool_job_t* job =
        (_thrdpool_job_t*)calloc(1, sizeof(_thrdpool_job_t));
    if (!job) {
        return -1;
    }
    job->routine = routine;
    job->arg = arg;

    uint32_t idx =
        atomic_fetch_add(&pool->next, 1) % (uint32_t)pool->nworkers;
    _thrdpool_worker_t* w = &pool->workers[idx];

    mpsc_push(&w->queue, &job->node);
    platform_sem_post(w->sem);
    return 0;
}

void thrdpool_destroy(thrdpool_t* restrict pool) {
    if (!pool) {
        return;
    }

    atomic_store(&pool->running, false);

    /* Wake all workers so they can exit. */
    for (int i = 0; i < pool->nworkers; i++) {
        platform_sem_post(pool->workers[i].sem);
    }

    for (int i = 0; i < pool->nworkers; i++) {
        thrd_join(pool->workers[i].thread, NULL);
    }

    for (int i = 0; i < pool->nworkers; i++) {
        platform_sem_destroy(pool->workers[i].sem);
    }
    free(pool->workers);
    free(pool);
}
