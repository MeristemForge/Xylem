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

typedef struct xylem_queue_s xylem_queue_t;

/**
 * @brief Create a FIFO queue.
 *
 * @return New queue, or NULL on allocation failure.
 */
extern xylem_queue_t* xylem_queue_create(void);

/**
 * @brief Destroy the queue and free all associated memory.
 *
 * @param queue  Queue to destroy.
 */
extern void xylem_queue_destroy(xylem_queue_t* queue);

/**
 * @brief Check whether the queue contains any elements.
 *
 * @param queue  Queue to check.
 * @return true if the queue is empty, false otherwise.
 */
extern bool xylem_queue_empty(xylem_queue_t* queue);

/**
 * @brief Return the number of elements in the queue.
 *
 * @param queue  Queue to query.
 * @return Number of elements.
 */
extern size_t xylem_queue_len(xylem_queue_t* queue);

/**
 * @brief Enqueue a data element at the back.
 *
 * @param queue  Queue to enqueue into.
 * @param data   Pointer to the data element.
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_queue_enqueue(xylem_queue_t* queue, void* data);

/**
 * @brief Return the front element without removing it.
 *
 * @param queue  Queue to query.
 * @return Pointer to the data, or NULL if the queue is empty.
 */
extern void* xylem_queue_front(xylem_queue_t* queue);

/**
 * @brief Remove the front element.
 *
 * No-op if the queue is empty.
 *
 * @param queue  Queue to dequeue from.
 */
extern void xylem_queue_dequeue(xylem_queue_t* queue);

/**
 * @brief Remove all elements from the queue.
 *
 * @param queue  Queue to clear.
 */
extern void xylem_queue_clear(xylem_queue_t* queue);

/**
 * @brief Swap the contents of two queues in O(1).
 *
 * @param a  First queue.
 * @param b  Second queue.
 */
extern void xylem_queue_swap(xylem_queue_t* a, xylem_queue_t* b);
