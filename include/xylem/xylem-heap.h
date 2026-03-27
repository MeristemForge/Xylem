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

#define xylem_heap_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))

typedef struct xylem_heap_s      xylem_heap_t;
typedef struct xylem_heap_node_s xylem_heap_node_t;

struct xylem_heap_node_s {
    struct xylem_heap_node_s* left;
    struct xylem_heap_node_s* right;
    struct xylem_heap_node_s* parent;
};

struct xylem_heap_s {
    struct xylem_heap_node_s* root;
    size_t                    nelts;
    int (*cmp)(const xylem_heap_node_t* child, const xylem_heap_node_t* parent);
};

/**
 * @brief Initialize a binary min-heap.
 *
 * @param heap  Pointer to the heap structure to initialize.
 * @param cmp   Comparator function. Returns negative if child should be
 *              closer to root than parent (i.e., child has higher priority).
 */
extern void xylem_heap_init(xylem_heap_t* heap, int (*cmp)(const xylem_heap_node_t* child, const xylem_heap_node_t* parent));

/**
 * @brief Insert a node into the heap.
 *
 * @param heap  Pointer to the heap.
 * @param node  Pointer to the intrusive node to insert. The caller embeds
 *              this node in their own struct and recovers it with xylem_heap_entry().
 */
extern void xylem_heap_insert(xylem_heap_t* heap, xylem_heap_node_t* node);

/**
 * @brief Remove a specific node from the heap.
 *
 * @param heap  Pointer to the heap.
 * @param node  Pointer to the node to remove. Must be currently in the heap.
 */
extern void xylem_heap_remove(xylem_heap_t* heap, xylem_heap_node_t* node);

/**
 * @brief Remove the root (highest-priority) node from the heap.
 *
 * No-op if the heap is empty.
 *
 * @param heap  Pointer to the heap.
 */
extern void xylem_heap_dequeue(xylem_heap_t* heap);

/**
 * @brief Check whether the heap is empty.
 *
 * @param heap  Pointer to the heap.
 *
 * @return true if the heap contains no nodes, false otherwise.
 */
extern bool xylem_heap_empty(xylem_heap_t* heap);

/**
 * @brief Return the root (highest-priority) node without removing it.
 *
 * @param heap  Pointer to the heap.
 *
 * @return Pointer to the root node, or NULL if the heap is empty.
 */
extern xylem_heap_node_t* xylem_heap_root(xylem_heap_t* heap);