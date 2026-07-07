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

#include "container/queue.h"
#include "platform/platform-sem.h"
#include "xylem/xylem-threads.h"

#include <stdbool.h>
#include <stdlib.h>

#define DYNPOOL_DEFAULT_MAX_THREADS  512
#define DYNPOOL_DEFAULT_IDLE_TIMEOUT 10000

typedef struct _dynpool_job_s {
    queue_node_t node;
    void (*routine)(void*);
    void*       arg;
} _dynpool_job_t;

struct dynpool_s {
    queue_t         queue;
    mtx_t           mtx;
    platform_sem_t* sem;
    int32_t         num_threads;
    int32_t         num_idle;
    int32_t         num_notify;
    int32_t         max_threads;
    uint64_t        idle_timeout;
    bool            running;
};

static int _dynpool_thread_entry(void* arg) {
    dynpool_t* pool = (dynpool_t*)arg;

    for (;;) {
        mtx_lock(&pool->mtx);

        queue_node_t* node = queue_dequeue(&pool->queue);
        if (node) {
            _dynpool_job_t* job = queue_entry(node, _dynpool_job_t, node);
            mtx_unlock(&pool->mtx);
            job->routine(job->arg);
            free(job);
            continue;
        }

        if (!pool->running) {
            pool->num_threads--;
            mtx_unlock(&pool->mtx);
            return 0;
        }

        pool->num_idle++;
        mtx_unlock(&pool->mtx);

        int rc = platform_sem_timedwait(pool->sem, pool->idle_timeout);

        mtx_lock(&pool->mtx);
        bool notified = false;
        if (pool->num_notify > 0) {
            pool->num_notify--;
            notified = true;
        }
        pool->num_idle--;

        bool retire =
            !pool->running ||
            (!notified && rc != 0 && queue_empty(&pool->queue));
        if (retire) {
            pool->num_threads--;
            mtx_unlock(&pool->mtx);
            return 0;
        }

        mtx_unlock(&pool->mtx);
    }
}

static void _dynpool_spawn_reserved(dynpool_t* pool) {
    thrd_t thr;
    if (thrd_create(&thr, _dynpool_thread_entry, pool) == thrd_success) {
        thrd_detach(thr);
        return;
    }

    mtx_lock(&pool->mtx);
    pool->num_threads--;
    bool notify = false;
    if (!queue_empty(&pool->queue) &&
        pool->num_idle > pool->num_notify) {
        pool->num_notify++;
        notify = true;
    }
    mtx_unlock(&pool->mtx);

    if (notify) {
        platform_sem_post(pool->sem);
    }
}

dynpool_t* dynpool_create(dynpool_opts_t* opts) {
    dynpool_t* pool = (dynpool_t*)calloc(1, sizeof(dynpool_t));
    if (!pool) {
        return NULL;
    }

    queue_init(&pool->queue);
    if (mtx_init(&pool->mtx, mtx_plain) != thrd_success) {
        free(pool);
        return NULL;
    }

    pool->sem = platform_sem_create(0);
    if (!pool->sem) {
        mtx_destroy(&pool->mtx);
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

    pool->running = true;
    return pool;
}

int dynpool_submit(dynpool_t* pool, void (*routine)(void*), void* arg) {
    if (!pool || !routine) {
        return -1;
    }

    _dynpool_job_t* job = (_dynpool_job_t*)calloc(1, sizeof(_dynpool_job_t));
    if (!job) {
        return -1;
    }

    job->routine = routine;
    job->arg = arg;

    mtx_lock(&pool->mtx);
    if (!pool->running) {
        mtx_unlock(&pool->mtx);
        free(job);
        return -1;
    }

    queue_enqueue(&pool->queue, &job->node);

    bool notify = false;
    if (pool->num_idle > pool->num_notify) {
        pool->num_notify++;
        notify = true;
    }

    bool spawn = false;
    if (!notify && pool->num_threads < pool->max_threads) {
        pool->num_threads++;
        spawn = true;
    }
    mtx_unlock(&pool->mtx);

    if (notify) {
        platform_sem_post(pool->sem);
    }
    if (spawn) {
        _dynpool_spawn_reserved(pool);
    }

    return 0;
}

void dynpool_destroy(dynpool_t* pool) {
    if (!pool) {
        return;
    }

    mtx_lock(&pool->mtx);
    pool->running = false;
    int32_t count = pool->num_threads;
    mtx_unlock(&pool->mtx);

    for (int32_t i = 0; i < count; i++) {
        platform_sem_post(pool->sem);
    }

    for (;;) {
        mtx_lock(&pool->mtx);
        count = pool->num_threads;
        mtx_unlock(&pool->mtx);
        if (count == 0) {
            break;
        }
        thrd_yield();
    }

    queue_node_t* node;
    while ((node = queue_dequeue(&pool->queue)) != NULL) {
        _dynpool_job_t* job = queue_entry(node, _dynpool_job_t, node);
        free(job);
    }

    platform_sem_destroy(pool->sem);
    mtx_destroy(&pool->mtx);
    free(pool);
}
