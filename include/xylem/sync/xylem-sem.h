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

typedef struct xylem_sem_s xylem_sem_t;

/**
 * Counting semaphore that bridges coroutines and OS threads.
 *
 * Unlike the other primitives in this directory (mutex, cond,
 * waitgroup, channel), which are coroutine-only and abort if a
 * blocking op is called off-coroutine, xylem_sem is the one sync
 * object whose blocking op is *any-context*: it is meant precisely
 * for a coroutine to notify an external thread, or an external thread
 * to notify a coroutine.
 *
 * Semantics:
 *   - Standard counting semaphore. wait() decrements; if the count is
 *     already zero the caller blocks until a post() hands it a token.
 *     post() wakes the FIFO-oldest waiter if any, otherwise increments
 *     the count.
 *   - Direct hand-off: when a waiter is queued, post() transfers the
 *     token straight to it and never touches the count, so no wakeup
 *     is ever lost.
 *
 * Threading:
 *   - wait() adapts to its caller. On a coroutine it parks the
 *     coroutine (the worker thread stays free); on any other thread it
 *     blocks that OS thread. Both kinds of waiter share one FIFO queue
 *     and one count.
 *   - post(), trywait(), create(), destroy() are all callable from any
 *     thread and any context (coroutine or not). They never park.
 *   - How a waiter is woken is decided by what the waiter is, not by
 *     who posts: a coroutine waiter is rescheduled, a thread waiter is
 *     released on an OS semaphore. The poster may be either.
 *
 * A coroutine waiter requires a running scheduler (it is woken via the
 * scheduler it was parked on); a thread waiter needs no runtime.
 */

/**
 * @brief Create a counting semaphore.
 *
 * Callable from any thread or context.
 *
 * @param value  Initial token count.
 *
 * @return Semaphore handle, or NULL on allocation failure.
 */
extern xylem_sem_t* xylem_sem_create(unsigned int value);

/**
 * @brief Destroy the semaphore and free its resources.
 *
 * Callable from any thread or context. The caller must ensure no
 * coroutine or thread is still blocked in wait() on this semaphore.
 *
 * @param sem  Semaphore handle, NULL is safe.
 */
extern void xylem_sem_destroy(xylem_sem_t* sem);

/**
 * @brief Acquire a token, blocking if none is available.
 *
 * If the count is positive it is decremented and the call returns
 * immediately. Otherwise the caller blocks until a post() hands it a
 * token, in FIFO order with all other waiters.
 *
 * Context-adaptive: parks the calling coroutine when invoked from a
 * coroutine, or blocks the OS thread when invoked from any other
 * thread.
 *
 * @param sem  Semaphore handle.
 */
extern void xylem_sem_wait(xylem_sem_t* sem);

/**
 * @brief Try to acquire a token without blocking.
 *
 * Callable from any thread or context; never blocks.
 *
 * @param sem  Semaphore handle.
 *
 * @return true if a token was acquired, false if the count was zero.
 */
extern bool xylem_sem_trywait(xylem_sem_t* sem);

/**
 * @brief Release a token.
 *
 * If a waiter is queued, the FIFO-oldest one is handed the token and
 * woken (a coroutine waiter is rescheduled, a thread waiter is
 * released); otherwise the count is incremented.
 *
 * Callable from any thread or context; never blocks.
 *
 * @param sem  Semaphore handle.
 */
extern void xylem_sem_post(xylem_sem_t* sem);
