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

#include <stddef.h>

typedef struct xylem_waitgroup_s xylem_waitgroup_t;

/**
 * Waitgroup concurrency model
 *
 * A waitgroup is a countdown latch: producers call add() to register
 * pending work and done() when each unit finishes; any number of
 * consumer coroutines call wait() to block until the counter hits
 * zero, at which point all parked waiters are woken in a single
 * broadcast.
 *
 * Threading:
 *   - All operations (create, destroy, add, done, wait) must be called
 *     from inside a coroutine on a scheduler worker; calling any of them
 *     outside a coroutine context aborts the process. wait() parks;
 *     add()/done() never park but are coroutine-only by contract.
 *   - wait() must be called from inside a coroutine on a scheduler
 *     worker (it parks). Multiple coroutines may wait() on the same
 *     waitgroup concurrently; they are all released together when
 *     the counter reaches zero.
 *
 * Misuse that aborts the process:
 *   - done() called more times than add() has ever promised
 *     (counter underflow). Matches Go's "negative WaitGroup counter"
 *     panic.
 */

/**
 * @brief Create a new waitgroup.
 *
 * @return Pointer to the new waitgroup, or NULL on allocation failure.
 */
extern xylem_waitgroup_t* xylem_waitgroup_create(void);

/**
 * @brief Increment the waitgroup counter.
 *
 * Thread-safe within the runtime. Typically called before spawning the
 * work units whose completion the counter tracks; calling add() after a
 * wait() may already be in progress is a logic error and is not
 * supported. Must be called from inside a coroutine (coroutine-only).
 *
 * @param waitgroup  Pointer to the waitgroup.
 * @param delta      Number of work items to add.
 */
extern void xylem_waitgroup_add(xylem_waitgroup_t* waitgroup, size_t delta);

/**
 * @brief Decrement the waitgroup counter by one.
 *
 * When the counter reaches zero, every coroutine parked
 * in xylem_waitgroup_wait() is resumed in FIFO order. Calling done()
 * more times than add() has promised aborts the process with a
 * diagnostic log.
 *
 * @param waitgroup  Pointer to the waitgroup.
 */
extern void xylem_waitgroup_done(xylem_waitgroup_t* waitgroup);

/**
 * @brief Suspend the current coroutine until the counter reaches zero.
 *
 * Returns immediately if the counter is already zero. Any number of
 * coroutines may wait() on the same waitgroup; they are all released
 * together by the done() that drops the counter to zero.
 *
 * @param waitgroup  Pointer to the waitgroup.
 */
extern void xylem_waitgroup_wait(xylem_waitgroup_t* waitgroup);

/**
 * @brief Destroy the waitgroup and free its resources.
 *
 * @param waitgroup  Pointer to the waitgroup, NULL is safe.
 */
extern void xylem_waitgroup_destroy(xylem_waitgroup_t* waitgroup);
