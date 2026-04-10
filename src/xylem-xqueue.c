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

#include "xylem/xylem-xqueue.h"

#include <stdlib.h>

typedef struct {
    xylem_queue_node_t node;
    void*              data;
} _xqueue_node_t;

static inline _xqueue_node_t* _xqueue_alloc_node(void* data) {
    _xqueue_node_t* n = malloc(sizeof(_xqueue_node_t));
    if (!n) {
        return NULL;
    }
    n->data = data;
    return n;
}

void xylem_xqueue_init(xylem_xqueue_t* queue) {
    xylem_queue_init(&queue->queue);
}

bool xylem_xqueue_empty(xylem_xqueue_t* queue) {
    return xylem_queue_empty(&queue->queue);
}

size_t xylem_xqueue_len(xylem_xqueue_t* queue) {
    return xylem_queue_len(&queue->queue);
}

int xylem_xqueue_enqueue(xylem_xqueue_t* queue, void* data) {
    _xqueue_node_t* n = _xqueue_alloc_node(data);
    if (!n) {
        return -1;
    }
    xylem_queue_enqueue(&queue->queue, &n->node);
    return 0;
}

void* xylem_xqueue_front(xylem_xqueue_t* queue) {
    xylem_queue_node_t* n = xylem_queue_front(&queue->queue);
    return n ? xylem_queue_entry(n, _xqueue_node_t, node)->data : NULL;
}

void xylem_xqueue_dequeue(xylem_xqueue_t* queue) {
    xylem_queue_node_t* n = xylem_queue_dequeue(&queue->queue);
    if (!n) {
        return;
    }
    free(xylem_queue_entry(n, _xqueue_node_t, node));
}

void xylem_xqueue_clear(xylem_xqueue_t* queue) {
    while (!xylem_queue_empty(&queue->queue)) {
        xylem_xqueue_dequeue(queue);
    }
}
