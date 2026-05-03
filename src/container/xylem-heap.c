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

#include "xylem/container/xylem-heap.h"
#include "container/heap.h"

#include <stdlib.h>

struct xylem_heap_s {
    heap_t              heap;
    xylem_heap_cmp_fn_t cmp;
};

typedef struct {
    heap_node_t         node;
    void*               data;
    xylem_heap_cmp_fn_t cmp;
} _heap_wrap_node_t;

static int _heap_cmp_bridge(
    const heap_node_t* child, const heap_node_t* parent) {
    _heap_wrap_node_t* a = heap_entry((heap_node_t*)child, _heap_wrap_node_t, node);
    _heap_wrap_node_t* b = heap_entry((heap_node_t*)parent, _heap_wrap_node_t, node);
    return a->cmp(a->data, b->data);
}

xylem_heap_t* xylem_heap_create(xylem_heap_cmp_fn_t cmp) {
    xylem_heap_t* h = (xylem_heap_t*)calloc(1, sizeof(xylem_heap_t));
    if (!h) {
        return NULL;
    }
    heap_init(&h->heap, _heap_cmp_bridge);
    h->cmp = cmp;
    return h;
}

void xylem_heap_destroy(xylem_heap_t* heap) {
    if (!heap) {
        return;
    }
    xylem_heap_clear(heap);
    free(heap);
}

bool xylem_heap_empty(xylem_heap_t* heap) {
    return heap_empty(&heap->heap);
}

size_t xylem_heap_len(xylem_heap_t* heap) {
    return heap->heap.nelts;
}

int xylem_heap_insert(xylem_heap_t* heap, void* data) {
    _heap_wrap_node_t* n = (_heap_wrap_node_t*)calloc(1, sizeof(_heap_wrap_node_t));
    if (!n) {
        return -1;
    }
    n->data = data;
    n->cmp  = heap->cmp;
    heap_insert(&heap->heap, &n->node);
    return 0;
}

void* xylem_heap_root(xylem_heap_t* heap) {
    heap_node_t* n = heap_peek(&heap->heap);
    return n ? heap_entry(n, _heap_wrap_node_t, node)->data : NULL;
}

void xylem_heap_dequeue(xylem_heap_t* heap) {
    heap_node_t* n = heap_peek(&heap->heap);
    if (!n) {
        return;
    }
    heap_dequeue(&heap->heap);
    free(heap_entry(n, _heap_wrap_node_t, node));
}

void xylem_heap_clear(xylem_heap_t* heap) {
    while (!heap_empty(&heap->heap)) {
        xylem_heap_dequeue(heap);
    }
}
