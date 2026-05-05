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

#include "runq.h"

#include "container/queue.h"
#include "c11-threads.h"

#include <stdlib.h>

typedef struct _runq_entry_s {
    queue_node_t node;
    mco_coro*    co;
} _runq_entry_t;

struct runq_s {
    queue_t q;
    mtx_t   lock;
};

runq_t* runq_create(void) {
    runq_t* rq = (runq_t*)calloc(1, sizeof(runq_t));
    if (!rq) {
        return NULL;
    }
    queue_init(&rq->q);
    mtx_init(&rq->lock, mtx_plain);
    return rq;
}

void runq_destroy(runq_t* rq) {
    if (!rq) {
        return;
    }
    mtx_destroy(&rq->lock);
    free(rq);
}

void runq_push(runq_t* rq, mco_coro* co) {
    _runq_entry_t* entry =
        (_runq_entry_t*)calloc(1, sizeof(_runq_entry_t));
    if (!entry) {
        return;
    }
    entry->co = co;

    mtx_lock(&rq->lock);
    queue_enqueue(&rq->q, &entry->node);
    mtx_unlock(&rq->lock);
}

void runq_push_batch(runq_t* rq, mco_coro** batch, int32_t count) {
    if (count <= 0) {
        return;
    }

    /* Pre-allocate all entries before taking the lock. */
    _runq_entry_t* entries =
        (_runq_entry_t*)calloc((size_t)count, sizeof(_runq_entry_t));
    if (!entries) {
        /* Fallback: push one by one. */
        for (int32_t i = 0; i < count; i++) {
            runq_push(rq, batch[i]);
        }
        return;
    }

    for (int32_t i = 0; i < count; i++) {
        entries[i].co = batch[i];
    }

    mtx_lock(&rq->lock);
    for (int32_t i = 0; i < count; i++) {
        queue_enqueue(&rq->q, &entries[i].node);
    }
    mtx_unlock(&rq->lock);
}

mco_coro* runq_pop(runq_t* rq) {
    mtx_lock(&rq->lock);
    queue_node_t* node = queue_dequeue(&rq->q);
    mtx_unlock(&rq->lock);

    if (!node) {
        return NULL;
    }
    _runq_entry_t* entry = queue_entry(node, _runq_entry_t, node);
    mco_coro* co = entry->co;
    free(entry);
    return co;
}
