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
    int               value;
    xylem_heap_node_t node;
} test_item_t;

static int _test_cmp_min(const xylem_heap_node_t* child, const xylem_heap_node_t* parent) {
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

/**
 * Helper: validate that the heap's tree structure is a complete binary tree
 * and that the heap-order property holds (parent <= children for min-heap).
 */
static bool _validate_heap(const xylem_heap_t* heap) {
    if (heap->root == NULL) {
        return true;
    }

    /* BFS to check completeness and heap order */
    xylem_heap_node_t* queue[1024];
    size_t             front = 0, back = 0;
    queue[back++] = heap->root;

    bool   seen_null = false;
    size_t count = 0;

    while (front < back) {
        xylem_heap_node_t* node = queue[front++];

        if (node == NULL) {
            seen_null = true;
            continue;
        }

        if (seen_null) {
            /* Non-null after null violates completeness */
            return false;
        }
        count++;

        /* Check heap order: child >= parent (min-heap) */
        if (node->left) {
            if (heap->cmp(node->left, node) < 0) {
                return false; // child < parent ¡ú invalid
            }
            queue[back++] = node->left;
        } else {
            queue[back++] = NULL;
        }

        if (node->right) {
            if (heap->cmp(node->right, node) < 0) {
                return false;
            }
            queue[back++] = node->right;
        } else {
            queue[back++] = NULL;
        }
    }
    return count == heap->nelts;
}

/**
 * Test xylem_heap_init: verifies that the heap is properly initialized
 * with a NULL root, zero element count, and the provided comparison function.
 */
static void test_heap_init(void) {
    xylem_heap_t heap;

    xylem_heap_init(&heap, _test_cmp_min);

    ASSERT(heap.root == NULL);
    ASSERT(heap.nelts == 0);
    ASSERT(heap.cmp == _test_cmp_min);
}

/**
 * Test xylem_heap_insert and xylem_heap_root: inserts a single node
 * and checks that it becomes the root.
 */
static void test_heap_insert_single(void) {
    xylem_heap_t heap;
    test_item_t item = {.value = 42};

    xylem_heap_init(&heap, _test_cmp_min);
    xylem_heap_insert(&heap, &item.node);

    ASSERT(heap.nelts == 1);
    ASSERT(xylem_heap_root(&heap) == &item.node);
    ASSERT(item.node.parent == NULL);
    ASSERT(item.node.left == NULL);
    ASSERT(item.node.right == NULL);
}

/**
 * Test xylem_heap_empty: checks that empty() returns true for a new
 * heap and false after insertion.
 */
static void test_heap_empty(void) {
    xylem_heap_t heap;
    test_item_t item = {.value = 10};

    xylem_heap_init(&heap, _test_cmp_min);
    ASSERT(xylem_heap_empty(&heap) == true);

    xylem_heap_insert(&heap, &item.node);
    ASSERT(xylem_heap_empty(&heap) == false);
}

/**
 * Test xylem_heap_insert multiple nodes: verifies heap property is
 * maintained after inserting several items into a min-heap.
 */
static void test_heap_insert_multiple(void) {
    xylem_heap_t heap;
    test_item_t items[] = {
        {.value = 3}, {.value = 2}, {.value = 1}, {.value = 2}};

    xylem_heap_init(&heap, _test_cmp_min);

    for (int i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        xylem_heap_insert(&heap, &items[i].node);
    }
    ASSERT(heap.nelts == 4);

    /* Root must be the smallest */
    test_item_t* root_item =
        xylem_heap_entry(xylem_heap_root(&heap), test_item_t, node);
    ASSERT(root_item->value == 1);
}

/**
 * Test repeated dequeue until empty.
 */
static void test_heap_dequeue_all(void) {
    xylem_heap_t heap;
    test_item_t  items[10];
    for (int i = 0; i < 10; ++i) {
        items[i].value = i * 2 + (i % 3); // Some random-ish pattern
    }

    xylem_heap_init(&heap, _test_cmp_min);
    for (int i = 0; i < 10; ++i) {
        xylem_heap_insert(&heap, &items[i].node);
    }

    int last_value = -1;
    while (!xylem_heap_empty(&heap)) {
        test_item_t* current =
            xylem_heap_entry(xylem_heap_root(&heap), test_item_t, node);
        ASSERT(current->value >= last_value); // Should be non-decreasing
        last_value = current->value;
        xylem_heap_dequeue(&heap);
    }
    ASSERT(heap.nelts == 0);
}

/**
 * Test removes a non-root node and
 * checks heap size and integrity.
 */
static void test_heap_remove_arbitrary(void) {
    xylem_heap_t heap;
    test_item_t items_down[] = {
        {.value = 1},
        {.value = 4},
        {.value = 2},
        {.value = 3},
        {.value = 6},
        {.value = 5}};

    xylem_heap_init(&heap, _test_cmp_min);
    /**
     * Final min-heap structure after inserting [1, 4, 2, 3, 6, 5]:
     *
     *           1                ¡û root (min)
     *         /   \
     *        4     2             ¡û level 1
     *       / \   /
     *      3   6 5               ¡û level 2 (last level, left-filled)
     *
     */
    for (int i = 0; i < sizeof(items_down) / sizeof(items_down[0]); ++i) {
        xylem_heap_insert(&heap, &items_down[i].node);
    }
    ASSERT(heap.nelts == 6);
    
    // Walk down;
    xylem_heap_remove(&heap, &items_down[1].node);
    ASSERT(heap.nelts == 5);

    while (!xylem_heap_empty(&heap)) {
        xylem_heap_dequeue(&heap);
    }

    test_item_t items_up[] = {
        {.value = 1},   // 0
        {.value = 2},   // 1
        {.value = 100}, // 2
        {.value = 3},   // 3
        {.value = 4},   // 4
        {.value = 200}, // 5 ¡û DELETE THIS
        {.value = 300}, // 6
        {.value = 5},   // 7
        {.value = 6},   // 8
        {.value = 7}    // 9 ¡û last node
    };
    /**
     * Final min-heap structure after inserting the following values in
     * level-order: [1, 2, 100, 3, 4, 200, 300, 5, 6, 7]
     *
     * This is a valid min-heap because every parent ¡Ü its children.
     *
     * Tree representation (complete binary tree, filled left to right):
     *
     *                     1                          ¡û root (minimum)
     *                /         \
     *               2           100                  ¡û level 1
     *             /   \        /    \
     *            3     4      200    300             ¡û level 2
     *           / \   /
     *          5   6 7                               ¡û level 3
     */
    for (int i = 0; i < sizeof(items_up) / sizeof(items_up[0]); ++i) {
        xylem_heap_insert(&heap, &items_up[i].node);
    }
    ASSERT(heap.nelts == 10);

    // Walk up;
    xylem_heap_remove(&heap, &items_up[5].node);
    ASSERT(heap.nelts == 9);
}

/**
 * Test structural integrity after many operations.
 */
static void test_heap_structure_integrity(void) {
    xylem_heap_t heap;
    test_item_t  items[15];
    for (int i = 0; i < 15; ++i) {
        items[i].value = 15 - i; // Insert 15,14,...,1 ¡ú reverse order
    }

    xylem_heap_init(&heap, _test_cmp_min);
    for (int i = 0; i < 15; ++i) {
        xylem_heap_insert(&heap, &items[i].node);
        ASSERT(_validate_heap(&heap)); // Validate after every insert!
    }

    /* Now dequeue half */
    for (int i = 0; i < 7; ++i) {
        xylem_heap_dequeue(&heap);
        ASSERT(_validate_heap(&heap));
    }

    /* Remove arbitrary middle node */
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