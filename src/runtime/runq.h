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

#include <stddef.h>

/* Opaque mutex-protected global run queue. */
typedef struct runq_s runq_t;
typedef struct runq_node_s runq_node_t;

struct runq_node_s {
    runq_node_t* next;
};

#define runq_entry(node, type, member) \
    ((type*)((char*)(node) - offsetof(type, member)))

/**
 * @brief Create a global run queue.
 *
 * Thread-safe MPMC FIFO protected by a mutex.
 *
 * @return Run queue handle, or NULL on failure.
 */
extern runq_t* runq_create(void);

/**
 * @brief Destroy a run queue and free its memory.
 *
 * The queue must be empty before destruction.
 *
 * @param runq  Run queue to destroy.
 */
extern void runq_destroy(runq_t* runq);

/**
 * @brief Push a node onto the run queue.
 *
 * Thread-safe: can be called from any thread.
 *
 * @param runq  Run queue.
 * @param node  Intrusive run queue node embedded in the caller's struct.
 *
 * @note node must be non-NULL, not currently queued, and remain valid until
 *       popped.
 */
extern void runq_push(runq_t* runq, runq_node_t* node);

/**
 * @brief Push multiple nodes onto the run queue.
 *
 * A non-positive count is ignored.
 *
 * @param runq   Run queue.
 * @param nodes  Array of pointers to intrusive run queue nodes.
 * @param count  Number of nodes in the array.
 *
 * @note For a positive count, nodes must point to distinct, non-NULL nodes
 *       that are not currently queued. They must remain valid until popped.
 */
extern void runq_push_batch(
    runq_t*       runq,
    runq_node_t** nodes,
    int           count);

/**
 * @brief Pop a node from the run queue.
 *
 * Thread-safe: can be called from any thread.
 *
 * @param runq  Run queue.
 *
 * @return Run queue node pointer, or NULL if empty.
 */
extern runq_node_t* runq_pop(runq_t* runq);

/**
 * @brief Pop a fair share of nodes from the run queue.
 *
 * Computes min(size / consumer_count + 1, size, nodes_cap) and removes that
 * many nodes under one lock acquisition.
 *
 * @param runq            Run queue.
 * @param nodes           Output buffer; non-NULL when nodes_cap is positive.
 * @param nodes_cap       Maximum number of nodes to dequeue.
 * @param consumer_count  Number of consumers sharing the queue.
 *
 * @return Number of nodes dequeued, or 0 for an empty queue or invalid limits.
 */
extern int runq_pop_fair(
    runq_t*       runq,
    runq_node_t** nodes,
    int           nodes_cap,
    int           consumer_count);
