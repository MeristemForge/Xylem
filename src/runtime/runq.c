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

#include <stdlib.h>

struct runq_s {
    runq_node_t* head;
    runq_node_t* tail;
    size_t       node_count;
    mtx_t        lock;
};

static void _runq_append(
    runq_t*      runq,
    runq_node_t* batch_head,
    runq_node_t* batch_tail,
    size_t       count) {
    if (runq->tail) {
        runq->tail->next = batch_head;
    } else {
        runq->head = batch_head;
    }
    runq->tail = batch_tail;
    runq->node_count += count;
}

static runq_node_t* _runq_detach(runq_t* runq, size_t count) {
    runq_node_t* batch_head = runq->head;
    runq_node_t* batch_tail = batch_head;

    for (size_t i = 1; i < count; i++) {
        batch_tail = batch_tail->next;
    }
    runq->head = batch_tail->next;
    if (!runq->head) {
        runq->tail = NULL;
    }
    runq->node_count -= count;
    batch_tail->next = NULL;
    return batch_head;
}

runq_t* runq_create(void) {
    runq_t* runq = (runq_t*)calloc(1, sizeof(runq_t));
    if (!runq) {
        return NULL;
    }
    if (mtx_init(&runq->lock, mtx_plain) != thrd_success) {
        free(runq);
        return NULL;
    }
    return runq;
}

void runq_destroy(runq_t* runq) {
    if (!runq) {
        return;
    }
    mtx_destroy(&runq->lock);
    free(runq);
}

void runq_push(runq_t* runq, runq_node_t* node) {
    node->next = NULL;

    mtx_lock(&runq->lock);
    _runq_append(runq, node, node, 1);
    mtx_unlock(&runq->lock);
}

void runq_push_batch(
    runq_t*       runq,
    runq_node_t** nodes,
    int           count) {
    if (count <= 0) {
        return;
    }

    for (int i = 1; i < count; i++) {
        nodes[i - 1]->next = nodes[i];
    }
    runq_node_t* batch_head = nodes[0];
    runq_node_t* batch_tail = nodes[count - 1];
    batch_tail->next = NULL;

    mtx_lock(&runq->lock);
    _runq_append(runq, batch_head, batch_tail, (size_t)count);
    mtx_unlock(&runq->lock);
}

runq_node_t* runq_pop(runq_t* runq) {
    mtx_lock(&runq->lock);
    runq_node_t* node = NULL;
    if (runq->head) {
        node = _runq_detach(runq, 1);
    }
    mtx_unlock(&runq->lock);
    return node;
}

int runq_pop_fair(
    runq_t*       runq,
    runq_node_t** nodes,
    int           nodes_cap,
    int           consumer_count) {
    if (nodes_cap <= 0 || consumer_count <= 0) {
        return 0;
    }

    mtx_lock(&runq->lock);
    size_t queued_count = runq->node_count;
    size_t take_count = queued_count / (size_t)consumer_count;
    if (take_count < queued_count) {
        take_count++;
    }
    if (take_count > (size_t)nodes_cap) {
        take_count = (size_t)nodes_cap;
    }
    if (take_count == 0) {
        mtx_unlock(&runq->lock);
        return 0;
    }

    runq_node_t* batch_head = _runq_detach(runq, take_count);
    mtx_unlock(&runq->lock);

    int          count = (int)take_count;
    runq_node_t* node  = batch_head;

    for (int i = 0; i < count; i++) {
        runq_node_t* next_node = node->next;
        node->next = NULL;
        nodes[i] = node;
        node = next_node;
    }
    return count;
}

