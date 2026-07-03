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
 * coroutines or OS threads call wait() to block until the counter hits
 * zero. When the counter reaches zero, all queued waiters are released.
 *
 * Threading:
 *   - All operations work from any context: a coroutine on a scheduler
 *     worker, or an external OS thread. wait() blocks the caller in the
 *     way that fits its context (a coroutine parks, an OS thread blocks
 *     on its per-thread wake object) until the counter hits zero.
 *     Coroutine and OS-thread waiters share one FIFO queue. add() and
 *     done() never wait for the counter.
 *
 * Misuse that aborts the process:
 *   - done() called more times than add() has ever promised
 *     (counter underflow). Matches Go's "negative WaitGroup counter"
 *     panic.
 *
 * Lifetime:
 *   - This object may wake coroutine waiters through the runtime
 *     scheduler. External OS threads must not call waitgroup APIs after
 *     xylem_shutdown() has been called. Stop and join those threads
 *     before shutdown, or make sure they touch no waitgroup once
 *     shutdown begins.
 */

/**
 * @brief Create a new waitgroup.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * @return Pointer to the new waitgroup, or NULL on allocation failure.
 */
extern xylem_waitgroup_t* xylem_waitgroup_create(void);

/**
 * @brief Increment the waitgroup counter.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Typically called before
 * spawning the work units whose completion the counter tracks; calling
 * add() while waiters from a previous round may still be blocked is a
 * logic error and is not supported.
 *
 * @param waitgroup  Pointer to the waitgroup.
 * @param delta      Number of work items to add.
 */
extern void xylem_waitgroup_add(xylem_waitgroup_t* waitgroup, size_t delta);

/**
 * @brief Decrement the waitgroup counter by one.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * When the counter reaches zero, every queued waiter in
 * xylem_waitgroup_wait() is released. Calling done() more times than
 * add() has promised aborts the process with a diagnostic log.
 * Callable from any context.
 *
 * @param waitgroup  Pointer to the waitgroup.
 */
extern void xylem_waitgroup_done(xylem_waitgroup_t* waitgroup);

/**
 * @brief Suspend the current coroutine, or block the current OS thread,
 *        until the counter reaches zero.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Returns immediately if the counter is already zero. Any number of
 * waiters (coroutines, threads, or a mix) may wait() on the same
 * waitgroup; all queued waiters are released by the done() that drops
 * the counter to zero.
 *
 * @param waitgroup  Pointer to the waitgroup.
 */
extern void xylem_waitgroup_wait(xylem_waitgroup_t* waitgroup);

/**
 * @brief Destroy the waitgroup and free its resources.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * The caller must ensure no coroutine or thread is blocked in wait()
 * and no add()/done() call is in flight on this waitgroup.
 *
 * @param waitgroup  Pointer to the waitgroup, NULL is safe.
 */
extern void xylem_waitgroup_destroy(xylem_waitgroup_t* waitgroup);
