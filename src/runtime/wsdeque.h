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

typedef struct mco_coro mco_coro;
typedef struct wsdeque_s wsdeque_t;

/**
 * @brief Create a work-stealing deque.
 *
 * Allocates a bounded circular buffer with capacity 2^log2_cap.
 *
 * @param log2_cap  Log2 of the capacity (e.g. 10 → 1024 slots).
 *
 * @return Deque handle, or NULL on failure.
 */
extern wsdeque_t* wsdeque_create(uint32_t log2_cap);

/**
 * @brief Destroy a work-stealing deque and free its memory.
 *
 * @param dq  Deque to destroy.
 */
extern void wsdeque_destroy(wsdeque_t* dq);

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
 * @brief Steal a coroutine from the deque (any thread).
 *
 * @param dq  Deque to steal from.
 *
 * @return Coroutine pointer, or NULL if empty or contended.
 */
extern mco_coro* wsdeque_steal(wsdeque_t* dq);
