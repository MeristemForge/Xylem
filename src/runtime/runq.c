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

#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdlib.h>

struct runq_s {
    queue_t          q;
    mtx_t            lock;
    _Atomic int32_t  len; /* lock-free length hint for spin/peek fast paths */
};

runq_t* runq_create(void) {
    runq_t* rq = (runq_t*)calloc(1, sizeof(runq_t));
    if (!rq) {
        return NULL;
    }
    queue_init(&rq->q);
    if (mtx_init(&rq->lock, mtx_plain) != thrd_success) {
        free(rq);
        return NULL;
    }
    return rq;
}

void runq_destroy(runq_t* rq) {
    if (!rq) {
        return;
    }
    mtx_destroy(&rq->lock);
    free(rq);
}

void runq_push(runq_t* rq, queue_node_t* node) {
    mtx_lock(&rq->lock);
    queue_enqueue(&rq->q, node);
    atomic_fetch_add(&rq->len, 1);
    mtx_unlock(&rq->lock);
}

void runq_push_batch(runq_t* rq, queue_node_t** nodes, int32_t count) {
    if (count <= 0) {
        return;
    }
    mtx_lock(&rq->lock);
    for (int32_t i = 0; i < count; i++) {
        queue_enqueue(&rq->q, nodes[i]);
    }
    atomic_fetch_add(&rq->len, count);
    mtx_unlock(&rq->lock);
}

queue_node_t* runq_pop(runq_t* rq) {
    mtx_lock(&rq->lock);
    queue_node_t* node = queue_dequeue(&rq->q);
    if (node) {
        atomic_fetch_sub(&rq->len, 1);
    }
    mtx_unlock(&rq->lock);
    return node;
}

int32_t runq_len_approx(runq_t* rq) {
    return atomic_load(&rq->len);
}


int32_t runq_pop_fair(
    runq_t* rq, queue_node_t** out, int32_t cap, int32_t nprocs) {
    if (cap <= 0 || nprocs <= 0) {
        return 0;
    }
    mtx_lock(&rq->lock);
    size_t  size = queue_len(&rq->q);
    int32_t grab = (int32_t)(size / (size_t)nprocs + 1);
    if (grab > (int32_t)size) {
        grab = (int32_t)size;
    }
    if (grab > cap) {
        grab = cap;
    }
    int32_t n = 0;
    while (n < grab) {
        queue_node_t* node = queue_dequeue(&rq->q);
        if (!node) {
            break;
        }
        out[n++] = node;
    }
    if (n > 0) {
        atomic_fetch_sub(&rq->len, n);
    }
    mtx_unlock(&rq->lock);
    return n;
}

