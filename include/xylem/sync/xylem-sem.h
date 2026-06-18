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
 *     Coroutine waiters are queued FIFO. OS-thread waiters contend on
 *     the count word and may barge when a token is posted, so FIFO is
 *     not a global fairness guarantee across OS threads.
 *   - Direct hand-off: when only coroutine waiters are queued, post()
 *     transfers the token to the FIFO-oldest coroutine and never
 *     touches the count. When OS threads are blocked, post() banks
 *     the token and wakes contenders so threads cannot starve.
 *
 * Threading:
 *   - wait() adapts to its caller. On a coroutine it parks the
 *     coroutine (the worker thread stays free); on any other thread it
 *     blocks that OS thread. Coroutine waiters share one FIFO queue;
 *     OS-thread waiters share the same count but compete through the
 *     platform futex/semaphore path. timedwait() is the same with a
 *     deadline.
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
 * @note [THREAD-SAFE]
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
 * @note [THREAD-SAFE]
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
 * @note [CONTEXT-ADAPTIVE]
 *
 * If the count is positive it is decremented and the call returns
 * immediately. Otherwise the caller blocks until a post() hands it a
 * token. Coroutine waiters are resumed FIFO; OS-thread waiters contend
 * for banked tokens and are not globally FIFO.
 *
 * Parks the calling coroutine when invoked from a coroutine, or blocks
 * the OS thread when invoked from any other thread.
 *
 * @param sem  Semaphore handle.
 */
extern void xylem_sem_wait(xylem_sem_t* sem);

/**
 * @brief Acquire a token, blocking up to @p timeout_ms milliseconds.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Like xylem_sem_wait, but gives up after the timeout elapses. A
 * timeout of 0 makes this a non-blocking attempt: it acquires a token
 * if one is immediately available, otherwise returns false at once
 * (never blocks, never parks).
 *
 * A coroutine caller parks with a scheduler timer; an external thread
 * blocks on its per-thread OS semaphore with the same timeout. Both
 * share the one count. Coroutine waiters are FIFO among themselves;
 * OS-thread waiters contend for banked tokens.
 *
 * @param sem         Semaphore handle.
 * @param timeout_ms  Maximum time to wait, in milliseconds. 0 means a
 *                    non-blocking try.
 *
 * @return true if a token was acquired, false if the timeout elapsed.
 */
extern bool xylem_sem_timedwait(xylem_sem_t* sem, uint64_t timeout_ms);

/**
 * @brief Release a token.
 *
 * @note [THREAD-SAFE]
 *
 * If only coroutine waiters are queued, the FIFO-oldest coroutine is
 * handed the token and woken. If OS threads are blocked, the token is
 * banked and contenders are woken; an OS thread may acquire it before
 * an older coroutine. With no waiters, the count is incremented.
 *
 * Callable from any thread or context; never blocks.
 *
 * @param sem  Semaphore handle.
 */
extern void xylem_sem_post(xylem_sem_t* sem);
