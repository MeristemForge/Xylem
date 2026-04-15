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

#include "xylem/xylem-thrdpool.h"
#include "xylem/xylem-queue.h"

#include "deprecated/c11-threads.h"

#include <stdbool.h>
#include <stdlib.h>

typedef struct _thrdpool_job_s _thrdpool_job_t;

struct _thrdpool_job_s {
    void (*routine)(void*);
    void*              arg;
    xylem_queue_node_t n;
};

struct xylem_thrdpool_s {
    thrd_t*       thrds;
    int           thrdcnt;
    xylem_queue_t queue;
    mtx_t         mtx;
    cnd_t         cnd;
    bool          running;
};

static int _thrdpool_thrdfunc(void* arg) {
    xylem_thrdpool_t* pool = arg;

    while (true) {
        _thrdpool_job_t* job = NULL;

        mtx_lock(&pool->mtx);
        while (pool->running && xylem_queue_empty(&pool->queue)) {
            cnd_wait(&pool->cnd, &pool->mtx);
        }
        /* Drain remaining jobs before exiting. */
        if (!pool->running && xylem_queue_empty(&pool->queue)) {
            mtx_unlock(&pool->mtx);
            break;
        }
        xylem_queue_node_t* node = xylem_queue_dequeue(&pool->queue);
        if (node) {
            job = xylem_queue_entry(node, _thrdpool_job_t, n);
        }
        mtx_unlock(&pool->mtx);

        if (job) {
            job->routine(job->arg);
            free(job);
        }
    }
    return 0;
}

xylem_thrdpool_t* xylem_thrdpool_create(int nthrds) {
    xylem_thrdpool_t* pool = malloc(sizeof(xylem_thrdpool_t));
    if (!pool) {
        return NULL;
    }
    xylem_queue_init(&pool->queue);
    if (mtx_init(&pool->mtx, mtx_plain) != thrd_success) {
        free(pool);
        return NULL;
    }
    if (cnd_init(&pool->cnd) != thrd_success) {
        mtx_destroy(&pool->mtx);
        free(pool);
        return NULL;
    }

    pool->thrdcnt = 0;
    pool->running = true;
    pool->thrds = malloc((size_t)nthrds * sizeof(thrd_t));
    if (!pool->thrds) {
        mtx_destroy(&pool->mtx);
        cnd_destroy(&pool->cnd);
        free(pool);
        return NULL;
    }
    for (int i = 0; i < nthrds; i++) {
        if (thrd_create(&pool->thrds[pool->thrdcnt], _thrdpool_thrdfunc, pool) == thrd_success) {
            pool->thrdcnt++;
        }
    }
    return pool;
}

int xylem_thrdpool_post(
    xylem_thrdpool_t* restrict pool, void (*routine)(void*), void* arg) {
    _thrdpool_job_t* job = malloc(sizeof(_thrdpool_job_t));
    if (!job) {
        return -1;
    }
    job->routine = routine;
    job->arg = arg;

    mtx_lock(&pool->mtx);
    xylem_queue_enqueue(&pool->queue, &job->n);
    cnd_signal(&pool->cnd);
    mtx_unlock(&pool->mtx);
    return 0;
}

void xylem_thrdpool_destroy(xylem_thrdpool_t* restrict pool) {
    if (!pool) {
        return;
    }
    mtx_lock(&pool->mtx);
    pool->running = false;
    cnd_broadcast(&pool->cnd);
    mtx_unlock(&pool->mtx);

    for (int i = 0; i < pool->thrdcnt; i++) {
        thrd_join(pool->thrds[i], NULL);
    }
    while (!xylem_queue_empty(&pool->queue)) {
        xylem_queue_node_t* node = xylem_queue_dequeue(&pool->queue);
        if (node) {
            free(xylem_queue_entry(node, _thrdpool_job_t, n));
        }
    }
    mtx_destroy(&pool->mtx);
    cnd_destroy(&pool->cnd);

    free(pool->thrds);
    free(pool);
}
