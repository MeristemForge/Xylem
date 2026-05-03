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

typedef struct xylem_heap_s xylem_heap_t;

/* Comparator. Returns negative if a has higher priority than b. */
typedef int (*xylem_heap_cmp_fn_t)(const void* a, const void* b);

/**
 * @brief Create a priority queue with O(log n) insert and dequeue.
 *
 * @param cmp  Comparator defining element priority.
 * @return New heap, or NULL on allocation failure.
 */
extern xylem_heap_t* xylem_heap_create(xylem_heap_cmp_fn_t cmp);

/**
 * @brief Destroy the heap and free all associated memory.
 *
 * @param heap  Heap to destroy.
 */
extern void xylem_heap_destroy(xylem_heap_t* heap);

/**
 * @brief Check whether the heap contains any elements.
 *
 * @param heap  Heap to check.
 * @return true if the heap is empty, false otherwise.
 */
extern bool xylem_heap_empty(xylem_heap_t* heap);

/**
 * @brief Return the number of elements in the heap.
 *
 * @param heap  Heap to query.
 * @return Number of elements.
 */
extern size_t xylem_heap_len(xylem_heap_t* heap);

/**
 * @brief Insert a data element into the heap.
 *
 * @param heap  Heap to insert into.
 * @param data  Pointer to the data element.
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_heap_insert(xylem_heap_t* heap, void* data);

/**
 * @brief Return the highest-priority element without removing it.
 *
 * @param heap  Heap to query.
 * @return Pointer to the data, or NULL if the heap is empty.
 */
extern void* xylem_heap_root(xylem_heap_t* heap);

/**
 * @brief Remove the highest-priority element.
 *
 * No-op if the heap is empty.
 *
 * @param heap  Heap to dequeue from.
 */
extern void xylem_heap_dequeue(xylem_heap_t* heap);

/**
 * @brief Remove all elements from the heap.
 *
 * @param heap  Heap to clear.
 */
extern void xylem_heap_clear(xylem_heap_t* heap);
