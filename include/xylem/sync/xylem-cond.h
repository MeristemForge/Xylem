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

typedef struct xylem_cond_s  xylem_cond_t;
typedef struct xylem_mutex_s xylem_mutex_t;

/**
 * Condition variable concurrency model
 *
 * A xylem_cond is a condition variable paired with xylem_mutex.
 * Semantics mirror pthread_cond: edge-triggered, no accumulation of
 * missed signals; callers must check the predicate in a while-loop to
 * handle spurious wakeups and bursts.
 *
 * Threading:
 *   - All operations work from any context: a coroutine on a scheduler
 *     worker, or an external OS thread. wait() blocks the caller in the
 *     way that fits its context (a coroutine parks, an OS thread blocks
 *     on a per-thread wake object) and re-acquires `mtx` on wake; the
 *     caller MUST currently hold `mtx`. signal()/broadcast() do not
 *     wait on cond state, though they may cooperative-yield in
 *     coroutine context.
 *
 * wait() atomically enqueues the caller on the cond's waiter list
 * and releases `mtx`, then blocks. On wake it re-acquires `mtx`
 * before returning. The "enqueue before unlock" order is what makes
 * the atomic release + sleep correct: any signaler serialised
 * through `mtx` cannot observe the released mutex until the waiter
 * is already linked and thus visible to signal()/broadcast().
 * signal() chooses the FIFO-oldest waiter; broadcast() drains queued
 * waiters in FIFO order. Waiters still re-acquire `mtx` after wake, so
 * return order is not guaranteed to be FIFO.
 *
 * Correct usage:
 *
 *     xylem_mutex_lock(m);
 *     while (!predicate()) {
 *         xylem_cond_wait(c, m);
 *     }
 *     xylem_mutex_unlock(m);
 *
 * And on the signalling side, holding the same mutex:
 *
 *     xylem_mutex_lock(m);
 *     modify_state();
 *     xylem_cond_signal(c);
 *     xylem_mutex_unlock(m);
 *
 * A signaler normally holds the same mutex as the waiter, giving the
 * standard lost-wakeup-free ordering. An external thread that cannot
 * take the mutex must avoid lost wakeups out of band: set an atomic
 * predicate flag *before* calling signal()/broadcast().
 *
 * Lifetime:
 *   - This object may wake coroutine waiters through the runtime
 *     scheduler captured at create() time. Create conds while the
 *     runtime scheduler is available if coroutine waiters will use
 *     them. External OS threads must not call cond APIs after
 *     xylem_shutdown() has been called. Stop and join those threads
 *     before shutdown, or make sure they touch no cond once shutdown
 *     begins.
 */

/**
 * @brief Create a new condition variable.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * If coroutine waiters will use this cond, create it while the runtime
 * scheduler is available.
 *
 * @return Pointer to the new cond, or NULL on allocation failure.
 */
extern xylem_cond_t* xylem_cond_create(void);

/**
 * @brief Destroy the cond and free its resources.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * This must be the final externally synchronized call on the handle.
 * The caller must ensure no coroutine or thread is blocked in wait()
 * and no signal()/broadcast() call is in flight. Destroying a cond
 * that still has waiters is a caller bug.
 *
 * @param cond  Pointer to the cond, NULL is safe.
 */
extern void xylem_cond_destroy(xylem_cond_t* cond);

/**
 * @brief Atomically release `mtx` and suspend the caller until
 *        the cond is signalled.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Callable from any context, holding `mtx`: a coroutine parks, an
 * external OS thread blocks on a per-thread wake object. On return,
 * `mtx` is held again. Callers must re-check the predicate in a
 * while-loop.
 *
 * @param cond  Pointer to the cond.
 * @param mtx   Mutex currently held by the caller.
 */
extern void xylem_cond_wait(xylem_cond_t* cond, xylem_mutex_t* mtx);

/**
 * @brief Wake one waiter, if any.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Callable from any context. If no one is currently parked on the cond
 * the call is a no-op (no permit is stored). If waiters are queued, the
 * FIFO-oldest waiter is woken. In coroutine context this call may
 * cooperative-yield for runtime fairness.
 *
 * @param cond  Pointer to the cond.
 */
extern void xylem_cond_signal(xylem_cond_t* cond);

/**
 * @brief Wake every waiter currently parked on the cond.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Callable from any context. Waiters observed at the moment broadcast()
 * acquires its internal guard are all resumed in FIFO wake order;
 * waiters that block after that point are not affected. In coroutine
 * context this call may cooperative-yield for runtime fairness.
 *
 * @param cond  Pointer to the cond.
 */
extern void xylem_cond_broadcast(xylem_cond_t* cond);
