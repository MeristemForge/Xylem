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

/* Opaque fixed-capacity SPMC FIFO work-stealing queue. */
typedef struct wsq_s wsq_t;

/**
 * @brief Create a FIFO work-stealing queue of pointers.
 *
 * The queue is a fixed power-of-two ring. One owner thread is the only
 * producer and publishes elements at the tail. The owner and any number of
 * thieves consume from the head with compare-and-swap.
 *
 * Head and tail are 64-bit monotonic counters; physical slots are selected by
 * masking their low bits. Slots are atomic so a losing consumer can safely
 * overlap a producer that reuses a slot after another consumer advances the
 * head. NULL is reserved as the empty result and cannot be pushed.
 *
 * @param cap  Capacity (must be a positive power of 2).
 *
 * @return Queue handle, or NULL on failure.
 */
extern wsq_t* wsq_create(int cap);

/**
 * @brief Destroy a queue and free its memory.
 *
 * No thread may access the queue while it is being destroyed.
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
extern int wsq_remaining(wsq_t* q);

/**
 * @brief Push an element onto the tail (owner thread only).
 *
 * Stores the element in its slot before publishing the incremented tail, so a
 * consumer that observes the new tail also observes the element.
 *
 * @param q     Queue to push onto.
 * @param elem  Element pointer to enqueue; must be non-NULL.
 *
 * @return 0 on success, -1 if full or elem is NULL.
 */
extern int wsq_push(wsq_t* q, void* elem);

/**
 * @brief Pop the oldest element from the head (owner thread only).
 *
 * Reads the oldest slot, then claims it by advancing the shared head with
 * compare-and-swap. A failed claim retries with the head value returned by the
 * failed compare-and-swap.
 *
 * @param q  Queue to pop from.
 *
 * @return Element pointer, or NULL if empty.
 */
extern void* wsq_pop(wsq_t* q);

/**
 * @brief Move up to half of the queued elements off the head (owner thread).
 *
 * Claims min(ceil(available / 2), elems_cap) oldest elements with one
 * successful head compare-and-swap. Used when a full local queue spills work
 * to the global queue.
 *
 * @param q          Queue to drain.
 * @param elems      Output buffer; non-NULL when elems_cap is positive.
 * @param elems_cap  Maximum number of elements to remove.
 *
 * @return Number of elements removed, or 0 if empty or elems_cap is not
 * positive.
 */
extern int wsq_pop_half(wsq_t* q, void** elems, int elems_cap);

/**
 * @brief Steal up to half of the queued elements from the head (any thread).
 *
 * Claims min(ceil(available / 2), elems_cap) oldest elements with one
 * successful head compare-and-swap. On contention, retries with the updated
 * head rather than returning a partial batch.
 *
 * @param q          Queue to steal from.
 * @param elems      Output buffer; non-NULL when elems_cap is positive.
 * @param elems_cap  Maximum number of elements to steal.
 *
 * @return Number of elements stolen, or 0 if empty or elems_cap is not
 * positive.
 */
extern int wsq_steal_half(wsq_t* q, void** elems, int elems_cap);
