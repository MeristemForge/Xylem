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
#include <stdint.h>

typedef struct sem_s sem_t;

/**
 * Counting semaphore that bridges coroutines and OS threads.
 *
 * Unlike the other primitives in this directory (mutex, cond,
 * waitgroup, channel), which are coroutine-only and abort if a
 * blocking op is called off-coroutine, sem_t is the one sync
 * object whose blocking op is *any-context*: it is meant precisely
 * for a coroutine to notify an external thread, or an external thread
 * to notify a coroutine.
 *
 * Semantics:
 *   - Standard counting semaphore. wait() decrements; if the count is
 *     already zero the caller blocks until a post() hands it a token.
 *     Coroutine and OS-thread waiters share one FIFO queue.
 *   - Direct hand-off: when waiters are queued, post() transfers the
 *     token to the FIFO-oldest waiter and never touches the count.
 *     With no waiters, post() increments the count.
 *
 * Threading:
 *   - wait() adapts to its caller. On a coroutine it parks the
 *     coroutine (the worker thread stays free); on any other thread it
 *     blocks that OS thread. Coroutine waiters share one FIFO queue;
 *     OS-thread waiters block on a per-thread wake object. timedwait()
 *     is the same with a deadline.
 *   - post(), timedwait(0), create(), destroy() are all callable from
 *     any thread and any context (coroutine or not). They never park.
 *   - How a waiter is woken is decided by what the waiter is, not by
 *     who posts: a coroutine waiter is rescheduled, a thread waiter is
 *     released on an OS semaphore. The poster may be either.
 *
 * A coroutine waiter requires a running scheduler (it is woken via the
 * scheduler it was parked on); a thread waiter needs no runtime.
 *
 * Lifetime:
 *   - This object may wake coroutine waiters through the runtime
 *     scheduler. External OS threads must not call semaphore APIs after
 *     xylem_shutdown() has been called. Stop and join those threads
 *     before shutdown, or make sure they touch no semaphore once
 *     shutdown begins.
 */

/**
 * @brief Create a counting semaphore.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Callable from any thread or context.
 *
 * @param value  Initial token count.
 *
 * @return Semaphore handle, or NULL on allocation failure.
 */
extern sem_t* sem_create(uint32_t value);

/**
 * @brief Destroy the semaphore and free its resources.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Callable from any thread or context. The caller must ensure no
 * coroutine or thread is still blocked in wait() on this semaphore.
 *
 * @param sem  Semaphore handle, NULL is safe.
 */
extern void sem_destroy(sem_t* sem);

/**
 * @brief Acquire a token, blocking if none is available.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * If the count is positive it is decremented and the call returns
 * immediately. Otherwise the caller blocks until a post() hands it a
 * token. Waiters are resumed FIFO across coroutine and OS-thread
 * callers.
 *
 * Parks the calling coroutine when invoked from a coroutine, or blocks
 * the OS thread when invoked from any other thread.
 *
 * @param sem  Semaphore handle.
 */
extern void sem_wait(sem_t* sem);

/**
 * @brief Acquire a token, blocking up to @p timeout_ms milliseconds.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Like sem_wait, but gives up after the timeout elapses. A
 * timeout of 0 makes this a non-blocking attempt: it acquires a token
 * if one is immediately available, otherwise returns false at once
 * (never blocks, never parks).
 *
 * A coroutine caller parks with a scheduler timer; an external thread
 * blocks on its per-thread OS semaphore with the same timeout. Both
 * share the one count and one FIFO waiter queue.
 *
 * @param sem         Semaphore handle.
 * @param timeout_ms  Maximum time to wait, in milliseconds. 0 means a
 *                    non-blocking try.
 *
 * @return true if a token was acquired, false if the timeout elapsed.
 */
extern bool sem_timedwait(sem_t* sem, uint64_t timeout_ms);

/**
 * @brief Release a token.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * If waiters are queued, the FIFO-oldest waiter is handed the token
 * and woken. With no waiters, the count is incremented.
 *
 * Callable from any thread or context; never blocks.
 *
 * @param sem  Semaphore handle.
 */
extern void sem_post(sem_t* sem);
