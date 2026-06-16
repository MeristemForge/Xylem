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

#include <stdint.h>

/* Opaque FIFO work-stealing run queue handle. */
typedef struct wsq_s wsq_t;

/**
 * @brief Create a FIFO work-stealing queue of pointers.
 *
 * Single-producer (the owner pushes at the tail) / multi-consumer (the owner
 * and thieves claim from the head with CAS). This mirrors Go's per-P runq:
 * a fixed FIFO ring, an owner tail, and an atomic head shared by stealers.
 *
 * Stores opaque element pointers; NULL is reserved as the empty result, so
 * callers must not push NULL.
 *
 * @param cap  Capacity (must be a power of 2).
 *
 * @return Queue handle, or NULL on failure.
 */
extern wsq_t* wsq_create(uint32_t cap);

/**
 * @brief Destroy a queue and free its memory.
 *
 * @param q  Queue to destroy.
 */
extern void wsq_destroy(wsq_t* q);

/**
 * @brief Return the number of free slots.
 *
 * Intended for the owner thread; the result may be momentarily stale when
 * thieves are draining the head concurrently (it can only under-report free
 * space, never over-report).
 *
 * @param q  Queue to query.
 *
 * @return Number of free slots.
 */
extern int32_t wsq_remaining(wsq_t* q);

/**
 * @brief Push an element onto the tail (owner thread only).
 *
 * @param q     Queue to push onto.
 * @param elem  Element pointer to enqueue; must be non-NULL.
 *
 * @return 0 on success, -1 if full.
 */
extern int wsq_push(wsq_t* q, void* elem);

/**
 * @brief Pop the oldest element from the head (owner thread only).
 *
 * FIFO: returns the least-recently pushed element. Shares the head with
 * thieves, so it resolves the race with a compare-and-swap.
 *
 * @param q  Queue to pop from.
 *
 * @return Element pointer, or NULL if empty.
 */
extern void* wsq_pop(wsq_t* q);

/**
 * @brief Move up to half of the queued elements off the head (owner thread).
 *
 * Takes roughly half from the oldest end. Used by the owner when overflowing
 * to the global queue, matching Go's runqputslow shape.
 *
 * @param q    Queue to drain.
 * @param out  Output buffer to receive elements.
 * @param cap  Capacity of the output buffer.
 *
 * @return Number of elements removed.
 */
extern int32_t wsq_pop_half(wsq_t* q, void** out, int32_t cap);

/**
 * @brief Steal up to half of the queued elements from the head (any thread).
 *
 * Atomically claims roughly half of the available items from the oldest end
 * and writes them into the output buffer.
 *
 * @param q    Queue to steal from.
 * @param out  Output buffer to receive stolen elements.
 * @param cap  Capacity of the output buffer.
 *
 * @return Number of elements stolen (0 if empty or contended).
 */
extern int32_t wsq_steal_half(wsq_t* q, void** out, int32_t cap);
