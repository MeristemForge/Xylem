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

typedef struct xylem_mutex_s xylem_mutex_t;

/**
 * Mutex concurrency model
 *
 * A xylem_mutex is a cross-context lock: ownership is held between
 * lock() and unlock() by whoever acquired it -- a coroutine or a plain
 * OS thread. A contended lock() parks the calling coroutine on the
 * scheduler, or blocks the calling OS thread on a per-thread wake
 * semaphore, instead of spinning the worker. Ownership is not tied to
 * any thread: unlock() hands the lock straight to the FIFO-oldest
 * waiter, so a coroutine may unlock a lock another coroutine or thread
 * acquired.
 *
 * Threading:
 *   - All operations work from any context: a coroutine on a scheduler
 *     worker, or an external OS thread. lock() blocks the caller in the
 *     way that fits its context (park vs OS-thread block); the others
 *     never block. A coroutine waiter is woken by the scheduler; a
 *     thread waiter by its OS semaphore. Coroutine and thread waiters
 *     may queue on the same mutex and notify each other.
 *
 * Lifetime:
 *   - This object may wake coroutine waiters through the runtime
 *     scheduler. External OS threads must not call mutex APIs after
 *     xylem_shutdown() has been called. Stop and join those threads
 *     before shutdown, or make sure they touch no mutex once shutdown
 *     begins.
 */

/**
 * @brief Create a new coroutine mutex.
 *
 * @note [THREAD-SAFE]
 *
 * @return Pointer to the new mutex, or NULL on allocation failure.
 */
extern xylem_mutex_t* xylem_mutex_create(void);

/**
 * @brief Acquire the mutex.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * If the mutex is already held, the caller blocks until the holder
 * calls xylem_mutex_unlock(): a coroutine is suspended (parked) on the
 * scheduler, an external OS thread blocks on a per-thread semaphore.
 *
 * @param mutex  Pointer to the mutex.
 */
extern void xylem_mutex_lock(xylem_mutex_t* mutex);

/**
 * @brief Try to acquire the mutex without blocking.
 *
 * @note [THREAD-SAFE]
 *
 * Attempts the uncontended fast path only: if the mutex is free it is
 * acquired and true is returned; if it is already held this returns
 * false immediately without blocking. A lock-free CAS, callable from
 * any context (coroutine or OS thread).
 *
 * @param mutex  Pointer to the mutex.
 * @return true if the mutex was acquired, false if it was already held.
 */
extern bool xylem_mutex_trylock(xylem_mutex_t* mutex);

/**
 * @brief Release the mutex.
 *
 * @note [THREAD-SAFE]
 *
 * If anyone is waiting, the FIFO-oldest waiter (coroutine or thread) is
 * handed the lock directly. Callable from any context.
 *
 * @param mutex  Pointer to the mutex.
 */
extern void xylem_mutex_unlock(xylem_mutex_t* mutex);

/**
 * @brief Destroy the mutex and free its resources.
 *
 * @note [CALLER-SYNCHRONIZED]
 *
 * The caller must ensure no coroutine or thread is blocked in lock(),
 * currently owns the mutex, or is otherwise using this mutex.
 *
 * @param mutex  Pointer to the mutex, NULL is safe.
 */
extern void xylem_mutex_destroy(xylem_mutex_t* mutex);
