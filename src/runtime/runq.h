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

#include "container/queue.h"

#include <stdint.h>

typedef struct runq_s runq_t;

/**
 * @brief Create a global run queue.
 *
 * Thread-safe MPMC queue protected by a mutex.
 *
 * @return Run queue handle, or NULL on failure.
 */
extern runq_t* runq_create(void);

/**
 * @brief Destroy a run queue and free its memory.
 *
 * The queue must be empty before destruction.
 *
 * @param rq  Run queue to destroy.
 */
extern void runq_destroy(runq_t* rq);

/**
 * @brief Push a node onto the run queue.
 *
 * Thread-safe: can be called from any thread.
 *
 * @param rq    Run queue.
 * @param node  Intrusive queue node embedded in the caller's struct.
 */
extern void runq_push(runq_t* rq, queue_node_t* node);

/**
 * @brief Push multiple nodes onto the run queue in one lock acquisition.
 *
 * Thread-safe: can be called from any thread. More efficient than calling
 * runq_push in a loop when transferring a batch.
 *
 * @param rq     Run queue.
 * @param nodes  Array of pointers to intrusive queue nodes.
 * @param count  Number of nodes in the array.
 */
extern void runq_push_batch(runq_t* rq, queue_node_t** nodes, int32_t count);

/**
 * @brief Pop a node from the run queue.
 *
 * Thread-safe: can be called from any thread.
 *
 * @param rq  Run queue.
 *
 * @return Queue node pointer, or NULL if empty.
 */
extern queue_node_t* runq_pop(runq_t* rq);

/**
 * @brief Pop up to @p max nodes from the run queue in one lock acquisition.
 *
 * Thread-safe: can be called from any thread. More efficient than calling
 * runq_pop in a loop when a worker needs to refill its local deque.
 *
 * @param rq    Run queue.
 * @param out   Output array of pointers to intrusive queue nodes.
 * @param max   Maximum number of nodes to dequeue.
 *
 * @return Number of nodes actually dequeued (0 if the queue was empty).
 */
extern int32_t runq_pop_batch(runq_t* rq, queue_node_t** out, int32_t max);
