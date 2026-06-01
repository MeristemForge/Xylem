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

/** @brief Forward declaration for minicoro coroutine. */
typedef struct mco_coro mco_coro;

/** @brief Opaque work-stealing deque handle. */
typedef struct wsdeque_s wsdeque_t;

/**
 * @brief Create a work-stealing deque.
 *
 * @param cap  Capacity (must be a power of 2).
 *
 * @return Deque handle, or NULL on failure.
 */
extern wsdeque_t* wsdeque_create(uint32_t cap);

/**
 * @brief Destroy a work-stealing deque and free its memory.
 *
 * @param dq  Deque to destroy.
 */
extern void wsdeque_destroy(wsdeque_t* dq);

/**
 * @brief Return the number of free slots (owner thread only).
 *
 * @param dq  Deque to query.
 *
 * @return Number of free slots.
 */
extern int32_t wsdeque_remaining(wsdeque_t* dq);

/**
 * @brief Push a coroutine onto the deque (owner thread only).
 *
 * @param dq  Deque to push onto.
 * @param co  Coroutine to push.
 *
 * @return 0 on success, -1 if full.
 */
extern int wsdeque_push(wsdeque_t* dq, mco_coro* co);

/**
 * @brief Pop a coroutine from the deque (owner thread only).
 *
 * @param dq  Deque to pop from.
 *
 * @return Coroutine pointer, or NULL if empty.
 */
extern mco_coro* wsdeque_pop(wsdeque_t* dq);

/**
 * @brief Pop half of the coroutines from the deque (owner thread only).
 *
 * Removes up to half of the items from the oldest end (top) of the deque
 * and writes them into the output buffer. This is used to transfer work
 * to the global run queue when the local deque is full.
 *
 * @param dq   Deque to pop from.
 * @param out  Output buffer to receive coroutines.
 * @param cap  Capacity of the output buffer.
 *
 * @return Number of coroutines removed.
 */
extern int32_t wsdeque_pop_half(wsdeque_t* dq, mco_coro** out, int32_t cap);

/**
 * @brief Steal half of the coroutines from the deque (any thread).
 *
 * Atomically takes up to half of the available items from the top of
 * the deque and writes them into the output buffer.
 *
 * @param dq   Deque to steal from.
 * @param out  Output buffer to receive stolen coroutines.
 * @param cap  Capacity of the output buffer.
 *
 * @return Number of coroutines stolen (0 if empty or contended).
 */
extern int32_t wsdeque_steal_half(wsdeque_t* dq, mco_coro** out, int32_t cap);
