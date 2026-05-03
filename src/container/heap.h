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

_Pragma("once")

#include <stdbool.h>
#include <stddef.h>

typedef struct heap_node_s {
    struct heap_node_s* left;
    struct heap_node_s* right;
    struct heap_node_s* parent;
} heap_node_t;

typedef struct heap_s {
    heap_node_t* root;
    size_t       nelts;
    int (*cmp)(const heap_node_t* child, const heap_node_t* parent);
} heap_t;

#define heap_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))

/**
 * @brief Initialize a binary min-heap.
 *
 * @param heap  Pointer to the heap structure to initialize.
 * @param cmp   Comparator function. Returns negative if child should be
 *              closer to root than parent (i.e., child has higher priority).
 */
extern void heap_init(heap_t* heap, int (*cmp)(const heap_node_t* child, const heap_node_t* parent));

/**
 * @brief Insert a node into the heap.
 *
 * @param heap  Pointer to the heap.
 * @param node  Pointer to the intrusive node to insert. The caller embeds
 *              this node in their own struct and recovers it with heap_entry().
 */
extern void heap_insert(heap_t* heap, heap_node_t* node);

/**
 * @brief Remove a specific node from the heap.
 *
 * @param heap  Pointer to the heap.
 * @param node  Pointer to the node to remove. Must be currently in the heap.
 */
extern void heap_remove(heap_t* heap, heap_node_t* node);

/**
 * @brief Remove the root (highest-priority) node from the heap.
 *
 * No-op if the heap is empty.
 *
 * @param heap  Pointer to the heap.
 */
extern void heap_dequeue(heap_t* heap);

/**
 * @brief Check whether the heap is empty.
 *
 * @param heap  Pointer to the heap.
 *
 * @return true if the heap contains no nodes, false otherwise.
 */
extern bool heap_empty(heap_t* heap);

/**
 * @brief Return the root (highest-priority) node without removing it.
 *
 * @param heap  Pointer to the heap.
 *
 * @return Pointer to the root node, or NULL if the heap is empty.
 */
extern heap_node_t* heap_peek(heap_t* heap);