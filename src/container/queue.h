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

#include "container/list.h"

#include <stdbool.h>

typedef list_node_t queue_node_t;

typedef struct queue_s {
    list_node_t head;
    size_t      nelts;
} queue_t;

#define queue_entry(x, t, m) list_entry(x, t, m)

/**
 * @brief Initialize a FIFO queue.
 *
 * @param queue  Pointer to the queue structure to initialize.
 */
extern void queue_init(queue_t* queue);

/**
 * @brief Check whether the queue is empty.
 *
 * @param queue  Pointer to the queue.
 *
 * @return true if the queue contains no nodes, false otherwise.
 */
extern bool queue_empty(queue_t* queue);

/**
 * @brief Return the number of nodes in the queue.
 *
 * @param queue  Pointer to the queue.
 *
 * @return Number of nodes.
 */
extern size_t queue_len(queue_t* queue);

/**
 * @brief Enqueue a node at the back of the queue.
 *
 * @param queue  Pointer to the queue.
 * @param node   Pointer to the intrusive node to enqueue.
 */
extern void queue_enqueue(queue_t* queue, queue_node_t* node);

/**
 * @brief Dequeue and return the front node from the queue.
 *
 * @param queue  Pointer to the queue.
 *
 * @return Pointer to the dequeued node, or NULL if the queue is empty.
 */
extern queue_node_t* queue_dequeue(queue_t* queue);

/**
 * @brief Return the front node without removing it.
 *
 * @param queue  Pointer to the queue.
 *
 * @return Pointer to the front node, or NULL if the queue is empty.
 */
extern queue_node_t* queue_front(queue_t* queue);

/**
 * @brief Return the back node without removing it.
 *
 * @param queue  Pointer to the queue.
 *
 * @return Pointer to the back node, or NULL if the queue is empty.
 */
extern queue_node_t* queue_back(queue_t* queue);

/**
 * @brief Swap the contents of two queues in O(1).
 *
 * After the swap, queue1 contains what queue2 had and vice versa.
 *
 * @param queue1  Pointer to the first queue.
 * @param queue2  Pointer to the second queue.
 */
extern void queue_swap(queue_t* queue1, queue_t* queue2);
