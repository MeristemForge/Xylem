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

/**
 * Cross-context waiter
 *
 * The minimal shared core for representing one blocked party that may
 * be either a parked coroutine or a blocked OS thread, and waking it by
 * kind. Used directly by xylem_sem and the channel (a single atomic
 * receiver slot), and by the FIFO primitives (mutex / cond / waitgroup),
 * each of which keeps its own guarded list of waiters. This module owns
 * only what is identical across every cross-context primitive:
 *
 *   - the per-thread TLS wake semaphore,
 *   - wake dispatch (coroutine via scheduler_schedule, thread via
 *     platform_sem_post).
 *
 * Wake kind is decided by *what the waiter is*, not by who wakes it.
 * Queueing, locking, and any timeout machinery live in the consumer,
 * not here.
 */

#include "container/list.h"
#include "platform/platform-sem.h"
#include "runtime/scheduler.h"

#include "runtime/minicoro/minicoro.h"

typedef enum waiter_kind_e {
    WAITER_CO,
    WAITER_THR,
} waiter_kind_t;

/**
 * A blocked party. The caller embeds `node` in its own queue if it
 * keeps a list of waiters (the channel uses a single atomic slot and
 * leaves `node` unused).
 */
typedef struct waiter_s {
    list_node_t     node;
    waiter_kind_t   kind;
    mco_coro*       co;    /* WAITER_CO  */
    scheduler_t*    sched; /* WAITER_CO  */
    platform_sem_t* tsem;  /* WAITER_THR */
} waiter_t;

/**
 * @brief Fill @p w with the current execution context's wake identity.
 *
 * Captures whatever is needed to wake the caller later via waiter_wake,
 * setting `kind` to discriminate the two cases:
 *   - Coroutine (WAITER_CO): sets sched. The coroutine handle (`co`) is
 *     *not* captured here; it must be filled by the scheduler_park
 *     callback, after the coroutine has actually suspended, so a waker
 *     can never observe the coroutine pointer before the yield parks it.
 *   - Thread (WAITER_THR): sets tsem, the per-thread (TLS) wake
 *     semaphore. On OOM `tsem` is NULL, in which case the caller must
 *     not enqueue the waiter and cannot block (it could never be woken).
 *
 * Callers branch on `w->kind` (and thread callers additionally check
 * `w->tsem` for the OOM case).
 *
 * @param w  Waiter storage to initialise.
 */
extern void waiter_init(waiter_t* w);

/**
 * @brief Wake a waiter, dispatching by kind (coroutine via the
 *        scheduler, thread via its platform sem).
 *
 * @param w  A by-value copy of the waiter taken under the lock (or via
 *           the atomic exchange that claims it); never the (possibly
 *           freed) waiter storage itself. The waiter's storage is often
 *           a stack frame that may be gone the instant it resumes, so
 *           it must not be dereferenced after the lock is dropped --
 *           pass a copy, not a pointer.
 */
extern void waiter_wake(waiter_t w);

