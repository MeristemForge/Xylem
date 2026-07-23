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

#include "platform/platform-poller.h"
#include "container/heap.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declaration for minicoro coroutine. */
typedef struct mco_coro mco_coro;

/* Opaque coroutine scheduler handle. */
typedef struct scheduler_s scheduler_t;

/* Scheduler timer handle. Defined below once scheduler_timer_fn_t exists. */
typedef struct scheduler_timer_s scheduler_timer_t;

/* Opaque iowait slab allocator handle. */
typedef struct iowait_slab_s iowait_slab_t;

/**
 * Park commit callback invoked after a coroutine yields.
 *
 * Called on the worker thread with the suspended coroutine. If the
 * callback returns true, it has published the waiter and the coroutine
 * remains waiting. If false, the callback must not have published the
 * waiter and the worker makes the coroutine runnable again.
 *
 * co   The suspended coroutine.
 * arg  User data from scheduler_coro_park().
 *
 * Returns true to park, false to reschedule immediately.
 */
typedef bool (*scheduler_coro_park_commit_fn_t)(mco_coro* co, void* arg);

/**
 * Timer expiry callback.
 *
 * timer  The timer that fired.
 * ud     User data from scheduler_timer_start().
 */
typedef void (*scheduler_timer_fn_t)(scheduler_timer_t* timer, void* ud);

/**
 * Optional reference-count hooks for the timer's user data (ud).
 *
 * When installed via scheduler_timer_set_ud_guard(), the scheduler invokes
 * ud_ref the instant it commits to dispatching a fire -- inside the
 * owner worker's timer_lock, the same critical section that pulls the
 * timer out of the heap -- and ud_unref once the callback has returned
 * (or once a spawn that would have run it has failed). This pins ud
 * across the gap between dispatch and callback execution, closing the
 * use-after-free window where a concurrent teardown on another thread
 * could free ud after dispatch but before the callback first touches it.
 *
 * The hooks run on the scheduler's hot path, and ud_ref additionally
 * runs under timer_lock. They MUST be trivial and non-reentrant -- a
 * plain atomic refcount bump/drop. They must not take locks, arm or stop
 * timers, or otherwise re-enter the scheduler.
 */
typedef void (*scheduler_timer_ud_fn_t)(void* ud);

/* Configuration for scheduler_create. */
typedef struct scheduler_opts_s {
    int32_t worker_count;      /* 0 = use CPU count. */
    size_t  coro_stack_size;   /* 0 = use default (128 KB). */
} scheduler_opts_t;

/**
 * @brief Create a coroutine scheduler with N worker threads.
 *
 * Initializes the poller, timer heap, and worker pool internally.
 *
 * @param opts  Configuration, or NULL for defaults.
 *
 * @return Scheduler handle, or NULL on failure.
 */
extern scheduler_t* scheduler_create(scheduler_opts_t* opts);

/**
 * @brief Destroy the scheduler, joining all workers.
 *
 * Signals shutdown, wakes all workers, waits for them to stop,
 * then frees all resources.
 * Must not be called from one of this scheduler's workers.
 * Must not race with scheduler_stop() or scheduler_destroy() on the
 * same scheduler; runtime teardown serializes those calls.
 *
 * @param sched  Scheduler to destroy, or NULL (no-op).
 */
extern void scheduler_destroy(scheduler_t* sched);

/**
 * @brief Stop the scheduler without freeing its resources.
 *
 * Sets the running flag to false and wakes every worker. A caller outside this
 * scheduler then joins every worker; after that call returns, no coroutine on
 * the scheduler can run. A caller on one of this scheduler's workers only
 * initiates the stop and returns without joining, so that call is not a
 * teardown barrier; a later non-worker stop or destroy must join the workers.
 *
 * The scheduler's runq, poller, and worker structures remain allocated so late
 * cross-thread callers (for instance dynpool threads finishing a blocking task
 * with scheduler_coro_ready) can safely observe the stopped state and return
 * without UAF.
 *
 * Once stopping begins, coroutine spawn/ready and timer create/start/reset
 * reject or ignore new work. Timer stop/destroy remain valid until the
 * scheduler is destroyed.
 * scheduler_destroy() must still be called afterwards to free the
 * memory. Idempotent for sequential callers: calling it a second time
 * (or calling scheduler_destroy without calling stop first) is safe.
 * Must not race with another scheduler_stop() or scheduler_destroy()
 * call on the same scheduler.
 *
 * @param sched  Scheduler to stop, or NULL (no-op).
 */
extern void scheduler_stop(scheduler_t* sched);

/**
 * @brief Make a waiting coroutine runnable.
 *
 * Thread-safe, may be called from any thread. When called from a
 * scheduler worker thread, takes a fast path through the worker's
 * runnext slot / local deque, with overflow spilling to the global
 * runq. When called from any other thread (including workers of a
 * different scheduler), pushes to the global runq and wakes a
 * worker.
 *
 * @param sched  Scheduler handle.
 * @param co     Waiting coroutine to make runnable.
 *
 * @note No-op after scheduler_stop() begins. Must not race with
 *       scheduler_destroy().
 */
extern void scheduler_coro_ready(scheduler_t* sched, mco_coro* co);

/**
 * @brief Make a batch of waiting coroutines runnable with a single wake.
 *
 * Normally pushes the whole batch to the global runq and performs at most one
 * wake, amortising lock + signal costs. On a single-worker scheduler, a call
 * from that worker uses its local queue directly.
 *
 * Thread-safe. For a single coroutine, use scheduler_coro_ready.
 *
 * @param sched  Scheduler handle.
 * @param coros  Array of waiting coroutines. Must not be NULL when count > 0.
 * @param count  Number of coroutines in @p coros. No-op when count <= 0.
 *
 * @note No-op after scheduler_stop() begins. Must not race with
 *       scheduler_destroy().
 */
extern void scheduler_coro_ready_batch(
    scheduler_t* sched,
    mco_coro** coros,
    int count);

/**
 * @brief Spawn a new coroutine on the scheduler.
 *
 * Allocates the coroutine and makes it runnable.
 * When called from a worker thread it lands on that worker's local
 * path (runnext/deque); when called from any other thread it goes
 * to the global runq.
 *
 * @param sched  Scheduler handle.
 * @param fn     Coroutine entry function.
 * @param arg    Opaque argument.
 *
 * @return 0 on success, or -1 for an invalid entry, allocation failure, or a
 *         stopped scheduler.
 *
 * @note Must not race with scheduler_stop() or scheduler_destroy().
 */
extern int scheduler_coro_spawn(
    scheduler_t* sched,
    void (*fn)(void*),
    void* arg);

/**
 * @brief Suspend the current coroutine and commit an external waiter.
 *
 * MUST be called from inside a coroutine running on a scheduler
 * worker thread. After the coroutine yields, the worker changes it from
 * RUNNING to WAITING and invokes @p commit. A successful commit must publish
 * the waiter as its final shared operation and then only return true. A false
 * result must leave the waiter unpublished; the worker makes the coroutine
 * runnable again.
 *
 * @param sched   Scheduler handle.
 * @param commit  Commit callback invoked after the coroutine is waiting.
 * @param arg     Opaque argument passed to commit.
 */
extern void scheduler_coro_park(
    scheduler_t* sched,
    scheduler_coro_park_commit_fn_t commit,
    void* arg);

/**
 * @brief Consume cooperative scheduler credit for the current coroutine.
 *
 * Credit is refilled each time a coroutine is resumed. Long loops call this
 * after cooperative operations and yield when it returns true.
 *
 * @param cost  Operation cost to charge.
 *
 * @return true when the caller should yield, false otherwise.
 */
extern bool scheduler_coro_consume_credit(uint32_t cost);

/**
 * @brief Yield the current coroutine and requeue it immediately.
 *
 * The coroutine is placed back at the tail of the worker's run queue
 * so other coroutines get a chance to run. No-op outside a scheduler
 * coroutine.
 */
extern void scheduler_coro_yield(void);

/**
 * @brief Get the scheduler's poller handle.
 *
 * @param sched  Scheduler handle.
 *
 * @return Platform poller handle.
 */
extern platform_poller_sq_t* scheduler_get_poller(scheduler_t* sched);

/**
 * @brief Get the scheduler's iowait handle slab.
 *
 * @param sched  Scheduler handle.
 *
 * @return iowait handle slab owned by the scheduler.
 */
extern iowait_slab_t* scheduler_get_iowait_slab(scheduler_t* sched);

/**
 * @brief Create a timer attached to a scheduler.
 *
 * Thread-safe. The returned timer is inert until scheduler_timer_start()
 * arms it.
 *
 * @param sched  Scheduler handle.
 *
 * @return Timer handle, or NULL on failure.
 *
 * @note Returns NULL after scheduler_stop() begins. Must not race with
 *       scheduler_stop() or scheduler_destroy().
 */
extern scheduler_timer_t* scheduler_timer_create(scheduler_t* sched);

/**
 * @brief Set whether the timer callback runs in a spawned coroutine.
 *
 * @param timer  Timer handle.
 * @param spawn  true to run the callback in a coroutine, false to run inline.
 */
extern void scheduler_timer_set_spawn(scheduler_timer_t* timer, bool spawn);

/**
 * @brief Install reference-count hooks for the timer's user data (ud).
 *
 * Must be called before scheduler_timer_start(), while the timer is inert.
 * Pass NULL for both to disable (the default). See scheduler_timer_ud_fn_t
 * for the contract the hooks must satisfy and the race they close.
 *
 * @param timer  Timer handle.
 * @param ref    Invoked under timer_lock at dispatch to pin ud.
 * @param unref  Invoked after the callback returns to release ud.
 */
extern void scheduler_timer_set_ud_guard(
    scheduler_timer_t*      timer,
    scheduler_timer_ud_fn_t ref,
    scheduler_timer_ud_fn_t unref);

/**
 * @brief Destroy a timer. Stops it first if armed.
 *
 * Final externally synchronized release. This call stops the timer as a
 * cleanup fallback, then drops the creator's reference. It must not race
 * with scheduler_timer_start(), scheduler_timer_stop(),
 * scheduler_timer_reset(), or another destroy on the same timer.
 * scheduler_timer_stop() is the concurrent stop boundary.
 *
 * A callback already in flight may still run to completion; the
 * scheduler keeps the timer object alive internally until that callback
 * returns. `ud`'s backing object lifetime is still the caller's
 * responsibility.
 *
 * @param timer  Timer handle, or NULL (no-op).
 */
extern void scheduler_timer_destroy(scheduler_timer_t* timer);

/**
 * @brief Start or reconfigure a timer. Thread-safe.
 *
 * If the timer is already queued, it is removed and re-inserted with the
 * new parameters. If a previous fire is currently running, start schedules
 * the next generation after that callback returns.
 * Use scheduler_timer_reset() to move an armed timer to a new deadline
 * while preserving cb/ud.
 * Repeat timers are re-queued only after the previous callback returns,
 * so callbacks for the same timer never overlap.
 *
 * When both scheduler_timer_stop() and scheduler_timer_start() are called
 * during the same callback, stop takes priority: the timer goes idle and
 * the start request is silently dropped.
 *
 * @param timer       Timer handle (must not be NULL).
 * @param cb          Callback to invoke on expiry.
 * @param ud          User data for callback.
 * @param timeout_ms  Delay in milliseconds.
 * @param repeat_ms   Repeat interval, 0 for one-shot.
 *
 * @note No-op after scheduler_stop() begins. Must not race with
 *       scheduler_stop() or scheduler_destroy().
 */
extern void scheduler_timer_start(
    scheduler_timer_t*   timer,
    scheduler_timer_fn_t cb,
    void*            ud,
    uint64_t         timeout_ms,
    uint64_t         repeat_ms);

/**
 * @brief Stop a running timer. Thread-safe.
 *
 * Returns true if a pending fire was cancelled: either the timer was
 * still in the heap, or a running callback had scheduled a deferred
 * reset. Returns false if the timer was already inactive, its callback
 * already dispatched with no deferred reset, or the timer never started.
 * If a repeat callback is currently running, stop still prevents the
 * callback completion path from re-queueing the timer.
 *
 * @param timer  Timer handle.
 *
 * @return true if a pending or deferred fire was cancelled.
 */
extern bool scheduler_timer_stop(scheduler_timer_t* timer);

/**
 * @brief Re-arm a timer with a new delay. Thread-safe.
 *
 * Preserves the cb and ud that scheduler_timer_start() last configured,
 * and restarts the timer's clock from now:
 *   - one-shot timers (repeat == 0) fire once, timeout_ms from now.
 *   - periodic timers (repeat != 0) fire next in timeout_ms and
 *     adopt timeout_ms as the new repeat interval for subsequent
 *     fires.
 *
 * If the timer was still pending, its queued fire is cancelled. If the
 * callback is currently running, reset is applied after that callback
 * returns. A second reset during the same callback overwrites the earlier
 * deferred reset. If it was inactive (never armed, callback already
 * dispatched), it is armed fresh.
 *
 * When both scheduler_timer_stop() and scheduler_timer_reset() are called
 * during the same callback (stop_pending and reset_pending both true),
 * stop takes priority: the timer goes idle and the reset request is
 * silently dropped.
 *
 * @param timer       Timer handle, previously armed with scheduler_timer_start().
 * @param timeout_ms  New delay in milliseconds. Also becomes the new
 *                    repeat interval for periodic timers.
 *
 * @return true if a queued fire was cancelled before it ran, or if an
 *         earlier deferred reset from the current fire was overwritten.
 *         Returns false after scheduler_stop() begins.
 *
 * @note Must not race with scheduler_stop() or scheduler_destroy().
 */
extern bool scheduler_timer_reset(scheduler_timer_t* timer, uint64_t timeout_ms);

/**
 * Callback invoked when all coroutines have exited.
 *
 * ud  User data from scheduler_set_idle_cb().
 */
typedef void (*scheduler_idle_fn_t)(void* ud);

/**
 * @brief Register a callback for when all coroutines have exited.
 *
 * The callback fires once when the alive coroutine count drops to zero.
 * Only one callback may be registered at a time. This must be called
 * before workers can observe the callback, or while the scheduler is
 * externally synchronized.
 *
 * @param sched  Scheduler handle.
 * @param cb     Callback, or NULL to clear.
 * @param ud     User data passed to cb.
 */
extern void scheduler_set_idle_cb(
    scheduler_t* sched, scheduler_idle_fn_t cb, void* ud);
