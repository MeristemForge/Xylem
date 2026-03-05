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

#include "xylem.h"

typedef struct {
    xylem_heap_node_t     node;
    void*                 data;
    xylem_xheap_cmp_fn_t  cmp;
} _xheap_node_t;

static int _xheap_cmp_bridge(
    const xylem_heap_node_t* child, const xylem_heap_node_t* parent) {
    _xheap_node_t* a = xylem_heap_entry((xylem_heap_node_t*)child, _xheap_node_t, node);
    _xheap_node_t* b = xylem_heap_entry((xylem_heap_node_t*)parent, _xheap_node_t, node);
    return a->cmp(a->data, b->data);
}

void xylem_xheap_init(xylem_xheap_t* heap, xylem_xheap_cmp_fn_t cmp) {
    xylem_heap_init(&heap->heap, _xheap_cmp_bridge);
    heap->cmp = cmp;
}

bool xylem_xheap_empty(xylem_xheap_t* heap) {
    return xylem_heap_empty(&heap->heap);
}

size_t xylem_xheap_len(xylem_xheap_t* heap) {
    return heap->heap.nelts;
}

int xylem_xheap_insert(xylem_xheap_t* heap, void* data) {
    _xheap_node_t* n = malloc(sizeof(_xheap_node_t));
    if (!n) return -1;
    n->data = data;
    n->cmp  = heap->cmp;
    xylem_heap_insert(&heap->heap, &n->node);
    return 0;
}

void* xylem_xheap_root(xylem_xheap_t* heap) {
    xylem_heap_node_t* n = xylem_heap_root(&heap->heap);
    return n ? xylem_heap_entry(n, _xheap_node_t, node)->data : NULL;
}

void xylem_xheap_dequeue(xylem_xheap_t* heap) {
    xylem_heap_node_t* n = xylem_heap_root(&heap->heap);
    if (!n) return;
    xylem_heap_dequeue(&heap->heap);
    free(xylem_heap_entry(n, _xheap_node_t, node));
}

void xylem_xheap_clear(xylem_xheap_t* heap) {
    while (!xylem_heap_empty(&heap->heap)) {
        xylem_xheap_dequeue(heap);
    }
}
