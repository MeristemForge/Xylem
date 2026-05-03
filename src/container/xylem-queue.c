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

#include "xylem/container/xylem-queue.h"
#include "container/queue.h"

#include <stdlib.h>

struct xylem_queue_s {
    queue_t queue;
};

typedef struct {
    queue_node_t node;
    void*        data;
} _queue_wrap_node_t;

static inline _queue_wrap_node_t* _queue_wrap_alloc(void* data) {
    _queue_wrap_node_t* n = (_queue_wrap_node_t*)calloc(1, sizeof(_queue_wrap_node_t));
    if (!n) {
        return NULL;
    }
    n->data = data;
    return n;
}

xylem_queue_t* xylem_queue_create(void) {
    xylem_queue_t* q = (xylem_queue_t*)calloc(1, sizeof(xylem_queue_t));
    if (!q) {
        return NULL;
    }
    queue_init(&q->queue);
    return q;
}

void xylem_queue_destroy(xylem_queue_t* queue) {
    if (!queue) {
        return;
    }
    xylem_queue_clear(queue);
    free(queue);
}

bool xylem_queue_empty(xylem_queue_t* queue) {
    return queue_empty(&queue->queue);
}

size_t xylem_queue_len(xylem_queue_t* queue) {
    return queue_len(&queue->queue);
}

int xylem_queue_enqueue(xylem_queue_t* queue, void* data) {
    _queue_wrap_node_t* n = _queue_wrap_alloc(data);
    if (!n) {
        return -1;
    }
    queue_enqueue(&queue->queue, &n->node);
    return 0;
}

void* xylem_queue_front(xylem_queue_t* queue) {
    queue_node_t* n = queue_front(&queue->queue);
    return n ? queue_entry(n, _queue_wrap_node_t, node)->data : NULL;
}

void xylem_queue_dequeue(xylem_queue_t* queue) {
    queue_node_t* n = queue_dequeue(&queue->queue);
    if (!n) {
        return;
    }
    free(queue_entry(n, _queue_wrap_node_t, node));
}

void xylem_queue_clear(xylem_queue_t* queue) {
    while (!queue_empty(&queue->queue)) {
        xylem_queue_dequeue(queue);
    }
}

void xylem_queue_swap(xylem_queue_t* a, xylem_queue_t* b) {
    queue_swap(&a->queue, &b->queue);
}
