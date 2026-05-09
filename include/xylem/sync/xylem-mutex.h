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

typedef struct xylem_mutex_s xylem_mutex_t;

/**
 * Mutex concurrency model
 *
 * A xylem_mutex is a coroutine-owned lock: ownership is held by a
 * coroutine (not an OS thread) between lock() and unlock(), and a
 * contending lock() parks the caller instead of spinning the worker.
 *
 * Threading:
 *   - create(), destroy(), unlock() are safe from any thread. unlock
 *     from an external thread is permitted so a coroutine may hand
 *     the lock off (the wakeup itself only needs a scheduler_schedule).
 *   - lock() must be called from inside a coroutine on a scheduler
 *     worker. This holds even on the uncontended fast path, because
 *     an acquire from a non-coroutine thread cannot be safely parked
 *     if contention arrives later and would leave the mutex in a
 *     state that only aborts on the next contended acquire. Calling
 *     lock() outside a coroutine context aborts the process.
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
