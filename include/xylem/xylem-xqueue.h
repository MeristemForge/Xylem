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

typedef struct xylem_xqueue_s xylem_xqueue_t;

struct xylem_xqueue_s {
    xylem_queue_t queue;
};

/**
 * @brief Initialize a non-intrusive FIFO queue.
 *
 * @param queue  Pointer to the queue structure to initialize.
 */
extern void xylem_xqueue_init(xylem_xqueue_t* queue);

/**
 * @brief Check whether the queue is empty.
 *
 * @param queue  Pointer to the queue.
 *
 * @return true if the queue contains no elements, false otherwise.
 */
extern bool xylem_xqueue_empty(xylem_xqueue_t* queue);

/**
 * @brief Return the number of elements in the queue.
 *
 * @param queue  Pointer to the queue.
 *
 * @return Number of elements.
 */
extern size_t xylem_xqueue_len(xylem_xqueue_t* queue);

/**
 * @brief Enqueue an element at the back of the queue.
 *
 * Allocates an internal node and stores the pointer to data.
 * The caller must ensure data remains valid while in the queue.
 *
 * @param queue  Pointer to the queue.
 * @param data   Pointer to the element data to store.
 *
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_xqueue_enqueue(xylem_xqueue_t* queue, void* data);

/**
 * @brief Return a pointer to the front element without removing it.
 *
 * @param queue  Pointer to the queue.
 *
 * @return Pointer to the element data, or NULL if the queue is empty.
 */
extern void* xylem_xqueue_front(xylem_xqueue_t* queue);

/**
 * @brief Dequeue and free the front element.
 *
 * No-op if the queue is empty.
 *
 * @param queue  Pointer to the queue.
 */
extern void xylem_xqueue_dequeue(xylem_xqueue_t* queue);

/**
 * @brief Remove and free all elements in the queue.
 *
 * @param queue  Pointer to the queue.
 */
extern void xylem_xqueue_clear(xylem_xqueue_t* queue);
