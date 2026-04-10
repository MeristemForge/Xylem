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
#include "assert.h"

typedef struct test_item_s {
    int32_t           value;
    xylem_heap_node_t node;
} test_item_t;

static int _test_cmp_min(
    const xylem_heap_node_t* child,
    const xylem_heap_node_t* parent) {
    const test_item_t* c = xylem_heap_entry(child, test_item_t, node);
    const test_item_t* p = xylem_heap_entry(parent, test_item_t, node);
    if (c->value < p->value) {
        return -1;
    }
    if (c->value > p->value) {
        return 1;
    }
    return 0;
}

/* BFS validation: completeness + heap-order property. */
static bool _validate_heap(const xylem_heap_t* heap) {
    if (heap->root == NULL) {
        return true;
    }

    xylem_heap_node_t* queue[1024];
    size_t             front = 0, back = 0;
    queue[back++] = heap->root;

    bool   seen_null = false;
    size_t count = 0;

    while (front < back) {
        xylem_heap_node_t* n = queue[front++];
        if (n == NULL) {
            seen_null = true;
            continue;
        }
        if (seen_null) {
            return false; /* non-null after null -> not complete */
        }
        count++;

        if (n->left) {
            if (heap->cmp(n->left, n) < 0) {
                return false;
            }
            queue[back++] = n->left;
        } else {
            queue[back++] = NULL;
        }
        if (n->right) {
            if (heap->cmp(n->right, n) < 0) {
                return false;
            }
            queue[back++] = n->right;
        } else {
            queue[back++] = NULL;
        }
    }
    return count == heap->nelts;
}

/* Init: NULL root, zero count, correct comparator. */
static void test_heap_init(void) {
    xylem_heap_t heap;
    xylem_heap_init(&heap, _test_cmp_min);
    ASSERT(heap.root == NULL);
    ASSERT(heap.nelts == 0);
    ASSERT(heap.cmp == _test_cmp_min);
}

/* Insert single node: becomes root. */
static void test_heap_insert_single(void) {
    xylem_heap_t heap;
    test_item_t  item = {.value = 42};

    xylem_heap_init(&heap, _test_cmp_min);
    xylem_heap_insert(&heap, &item.node);

    ASSERT(heap.nelts == 1);
    ASSERT(xylem_heap_root(&heap) == &item.node);
    ASSERT(item.node.parent == NULL);
    ASSERT(item.node.left == NULL);
    ASSERT(item.node.right == NULL);
}

/* Empty check before and after insert. */
static void test_heap_empty(void) {
    xylem_heap_t heap;
    test_item_t  item = {.value = 10};

    xylem_heap_init(&heap, _test_cmp_min);
    ASSERT(xylem_heap_empty(&heap) == true);

    xylem_heap_insert(&heap, &item.node);
    ASSERT(xylem_heap_empty(&heap) == false);
}

/* Multiple inserts: root must be the minimum. */
static void test_heap_insert_multiple(void) {
    xylem_heap_t heap;
    test_item_t  items[] = {
        {.value = 3}, {.value = 2}, {.value = 1}, {.value = 2}};

    xylem_heap_init(&heap, _test_cmp_min);
    for (int i = 0; i < 4; i++) {
        xylem_heap_insert(&heap, &items[i].node);
    }
    ASSERT(heap.nelts == 4);

    test_item_t* root =
        xylem_heap_entry(xylem_heap_root(&heap), test_item_t, node);
    ASSERT(root->value == 1);
}

/* Dequeue all: values come out in non-decreasing order. */
static void test_heap_dequeue_all(void) {
    xylem_heap_t heap;
    test_item_t  items[10];
    for (int i = 0; i < 10; i++) {
        items[i].value = i * 2 + (i % 3);
    }

    xylem_heap_init(&heap, _test_cmp_min);
    for (int i = 0; i < 10; i++) {
        xylem_heap_insert(&heap, &items[i].node);
    }

    int last = -1;
    while (!xylem_heap_empty(&heap)) {
        test_item_t* cur =
            xylem_heap_entry(xylem_heap_root(&heap), test_item_t, node);
        ASSERT(cur->value >= last);
        last = cur->value;
        xylem_heap_dequeue(&heap);
    }
    ASSERT(heap.nelts == 0);
}

/* Remove arbitrary (non-root) nodes. */
static void test_heap_remove_arbitrary(void) {
    xylem_heap_t heap;
    test_item_t  items[] = {
        {.value = 1}, {.value = 4}, {.value = 2},
        {.value = 3}, {.value = 6}, {.value = 5}};

    xylem_heap_init(&heap, _test_cmp_min);
    for (int i = 0; i < 6; i++) {
        xylem_heap_insert(&heap, &items[i].node);
    }
    ASSERT(heap.nelts == 6);

    /* Remove non-root node (value=4) */
    xylem_heap_remove(&heap, &items[1].node);
    ASSERT(heap.nelts == 5);

    /* Drain and verify order */
    while (!xylem_heap_empty(&heap)) {
        xylem_heap_dequeue(&heap);
    }

    /* Test walk-up case: remove a node whose replacement bubbles up */
    test_item_t items2[] = {
        {.value = 1},   {.value = 2},   {.value = 100},
        {.value = 3},   {.value = 4},   {.value = 200},
        {.value = 300}, {.value = 5},   {.value = 6},
        {.value = 7}};

    for (int i = 0; i < 10; i++) {
        xylem_heap_insert(&heap, &items2[i].node);
    }
    ASSERT(heap.nelts == 10);

    xylem_heap_remove(&heap, &items2[5].node); /* remove 200 */
    ASSERT(heap.nelts == 9);
}

/* Structural integrity: validate after every insert and dequeue. */
static void test_heap_structure_integrity(void) {
    xylem_heap_t heap;
    test_item_t  items[15];
    for (int i = 0; i < 15; i++) {
        items[i].value = 15 - i; /* reverse order */
    }

    xylem_heap_init(&heap, _test_cmp_min);
    for (int i = 0; i < 15; i++) {
        xylem_heap_insert(&heap, &items[i].node);
        ASSERT(_validate_heap(&heap));
    }

    for (int i = 0; i < 7; i++) {
        xylem_heap_dequeue(&heap);
        ASSERT(_validate_heap(&heap));
    }

    xylem_heap_remove(&heap, &items[5].node);
    ASSERT(_validate_heap(&heap));
}

int main(void) {
    test_heap_init();
    test_heap_insert_single();
    test_heap_empty();
    test_heap_insert_multiple();
    test_heap_dequeue_all();
    test_heap_remove_arbitrary();
    test_heap_structure_integrity();
    return 0;
}
