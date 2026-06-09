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
 * A xylem_mutex is a coroutine-owned lock: ownership is held by a
 * coroutine (not an OS thread) between lock() and unlock(), and a
 * contending lock() parks the caller instead of spinning the worker.
 *
 * Threading:
 *   - All operations (create, destroy, lock, trylock, unlock) must be
 *     called from inside a coroutine on a scheduler worker; calling any
 *     of them outside a coroutine context aborts the process. lock() and
 *     a contended path may park the caller; the others never park but are
 *     still coroutine-only by contract, so the whole primitive is usable
 *     only from within the runtime.
 */

/**
 * @brief Create a new coroutine mutex.
 *
 * @return Pointer to the new mutex, or NULL on allocation failure.
 */
extern xylem_mutex_t* xylem_mutex_create(void);

/**
 * @brief Acquire the mutex.
 *
 * If the mutex is already held, the calling coroutine is suspended
 * until the holder calls xylem_mutex_unlock().
 *
 * @param mutex  Pointer to the mutex.
 */
extern void xylem_mutex_lock(xylem_mutex_t* mutex);

/**
 * @brief Try to acquire the mutex without blocking.
 *
 * Attempts the uncontended fast path only: if the mutex is free it is
 * acquired and true is returned; if it is already held this returns
 * false immediately without parking the caller. It never suspends, but
 * is coroutine-only by contract (like the rest of the mutex API) and
 * aborts if called outside a coroutine context.
 *
 * @param mutex  Pointer to the mutex.
 * @return true if the mutex was acquired, false if it was already held.
 */
extern bool xylem_mutex_trylock(xylem_mutex_t* mutex);

/**
 * @brief Release the mutex.
 *
 * If other coroutines are waiting, the next one in FIFO order is resumed.
 *
 * @param mutex  Pointer to the mutex.
 */
extern void xylem_mutex_unlock(xylem_mutex_t* mutex);

/**
 * @brief Destroy the mutex and free its resources.
 *
 * @param mutex  Pointer to the mutex, NULL is safe.
 */
extern void xylem_mutex_destroy(xylem_mutex_t* mutex);
