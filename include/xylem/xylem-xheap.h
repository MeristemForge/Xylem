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

#include "xylem.h"

typedef int (*xylem_xheap_cmp_fn_t)(const void* a, const void* b);

typedef struct xylem_xheap_s xylem_xheap_t;

struct xylem_xheap_s {
    xylem_heap_t          heap;
    xylem_xheap_cmp_fn_t  cmp;
};

/**
 * @brief Initialize a non-intrusive binary min-heap.
 *
 * @param heap  Pointer to the heap structure to initialize.
 * @param cmp   Comparator function. Returns negative if a has higher
 *              priority than b (a should be closer to root).
 */
extern void xylem_xheap_init(xylem_xheap_t* heap, xylem_xheap_cmp_fn_t cmp);

/**
 * @brief Check whether the heap is empty.
 *
 * @param heap  Pointer to the heap.
 *
 * @return true if the heap contains no elements, false otherwise.
 */
extern bool xylem_xheap_empty(xylem_xheap_t* heap);

/**
 * @brief Return the number of elements in the heap.
 *
 * @param heap  Pointer to the heap.
 *
 * @return Number of elements.
 */
extern size_t xylem_xheap_len(xylem_xheap_t* heap);

/**
 * @brief Insert an element into the heap.
 *
 * Allocates an internal node and stores the pointer to data.
 * The caller must ensure data remains valid while in the heap.
 *
 * @param heap  Pointer to the heap.
 * @param data  Pointer to the element data to store.
 *
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_xheap_insert(xylem_xheap_t* heap, void* data);

/**
 * @brief Return a pointer to the root (highest-priority) element.
 *
 * @param heap  Pointer to the heap.
 *
 * @return Pointer to the element data, or NULL if the heap is empty.
 */
extern void* xylem_xheap_root(xylem_xheap_t* heap);

/**
 * @brief Remove the root (highest-priority) element.
 *
 * No-op if the heap is empty.
 *
 * @param heap  Pointer to the heap.
 */
extern void xylem_xheap_dequeue(xylem_xheap_t* heap);

/**
 * @brief Remove and free all elements in the heap.
 *
 * @param heap  Pointer to the heap.
 */
extern void xylem_xheap_clear(xylem_xheap_t* heap);
