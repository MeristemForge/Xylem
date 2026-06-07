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

#include "thrdpool.h"

#include "container/queue.h"
#include "platform/platform-sem.h"
#include "thrds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct _thrdpool_job_s {
    queue_node_t node;
    void (*routine)(void*);
    void* arg;
} _thrdpool_job_t;

struct thrdpool_s {
    thrd_t*         threads;
    int32_t         nthreads;
    queue_t         jobs;
    mtx_t           lock;
    platform_sem_t* sem;
    _Atomic bool    running;
};

static _thrdpool_job_t* _thrdpool_pop(thrdpool_t* pool) {
    mtx_lock(&pool->lock);
    queue_node_t* node = queue_dequeue(&pool->jobs);
    mtx_unlock(&pool->lock);

    if (!node) {
        return NULL;
    }
    return queue_entry(node, _thrdpool_job_t, node);
}

static int _thrdpool_worker_entry(void* arg) {
    thrdpool_t* pool = (thrdpool_t*)arg;

    while (atomic_load(&pool->running)) {
        platform_sem_wait(pool->sem);

        _thrdpool_job_t* job = _thrdpool_pop(pool);
        if (job) {
            job->routine(job->arg);
            free(job);
        }
    }

    _thrdpool_job_t* job;
    while ((job = _thrdpool_pop(pool)) != NULL) {
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

    pool->threads = (thrd_t*)calloc((size_t)nthrds, sizeof(thrd_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    pool->sem = platform_sem_create(0);
    if (!pool->sem) {
        free(pool->threads);
        free(pool);
        return NULL;
    }

    queue_init(&pool->jobs);
    mtx_init(&pool->lock, mtx_plain);
    atomic_store(&pool->running, true);
    pool->nthreads = 0;

    for (int i = 0; i < nthrds; i++) {
        if (thrd_create(&pool->threads[i],
                        _thrdpool_worker_entry,
                        pool) == thrd_success) {
            pool->nthreads++;
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

    mtx_lock(&pool->lock);
    queue_enqueue(&pool->jobs, &job->node);
    mtx_unlock(&pool->lock);

    platform_sem_post(pool->sem);
    return 0;
}

void thrdpool_destroy(thrdpool_t* restrict pool) {
    if (!pool) {
        return;
    }

    atomic_store(&pool->running, false);

    for (int32_t i = 0; i < pool->nthreads; i++) {
        platform_sem_post(pool->sem);
    }

    for (int32_t i = 0; i < pool->nthreads; i++) {
        thrd_join(pool->threads[i], NULL);
    }

    mtx_destroy(&pool->lock);
    platform_sem_destroy(pool->sem);
    free(pool->threads);
    free(pool);
}
