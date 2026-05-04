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

typedef struct mco_coro mco_coro;
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
 * @brief Push a coroutine onto the run queue.
 *
 * Thread-safe: can be called from any thread.
 *
 * @param rq  Run queue.
 * @param co  Coroutine to enqueue.
 */
extern void runq_push(runq_t* rq, mco_coro* co);

/**
 * @brief Pop a coroutine from the run queue.
 *
 * Thread-safe: can be called from any thread.
 *
 * @param rq  Run queue.
 *
 * @return Coroutine pointer, or NULL if empty.
 */
extern mco_coro* runq_pop(runq_t* rq);
