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

#include <stdbool.h>
#include <stdint.h>

typedef struct mco_coro    mco_coro;
typedef struct scheduler_s scheduler_t;

/**
 * @brief Callback invoked by the scheduler when a poller event fires.
 *
 * Called on a worker thread with the readiness mask and the user-data
 * pointer from the platform_poller_sqe registration.
 *
 * @param revents  Readiness mask (PLATFORM_POLLER_RD_OP / WR_OP).
 * @param ud       User data from the sqe registration.
 */
typedef void (*scheduler_poll_fn_t)(int revents, void* ud);

/**
 * @brief Park callback invoked after a coroutine yields.
 *
 * Called on the worker thread with the suspended coroutine. If the
 * callback returns true, the coroutine is parked (not rescheduled).
 * If false, the worker pushes it back to its local deque.
 *
 * @param co   The suspended coroutine.
 * @param arg  User data from scheduler_park().
 *
 * @return true to park, false to reschedule immediately.
 */
typedef bool (*scheduler_park_fn_t)(mco_coro* co, void* arg);

/**
 * @brief Suspend the current coroutine and invoke a park callback.
 *
 * Yields the running coroutine. After the yield, the worker calls fn(co, arg).
 * This ensures the coroutine is fully suspended before any wakeup source
 * can see its pointer, eliminating schedule-before-yield races.
 *
 * @param sched  Scheduler handle.
 * @param fn     Park callback invoked after yield.
 * @param arg    Opaque argument passed to fn.
 */
extern void scheduler_park(
    scheduler_t* sched, scheduler_park_fn_t fn, void* arg);

typedef struct {
    int32_t  nworkers;  /*< 0 = use CPU count. */
    uint32_t deque_cap; /*< 0 = use default (log2=10, 1024 slots). */
} scheduler_opts_t;

/**
 * @brief Create a coroutine scheduler with N worker threads.
 *
 * @param opts  Configuration, or NULL for defaults.
 *
 * @return Scheduler handle, or NULL on failure.
 */
extern scheduler_t* scheduler_create(scheduler_opts_t* opts);

/**
 * @brief Destroy the scheduler, joining all workers.
 *
 * @param sched  Scheduler to destroy.
 */
extern void scheduler_destroy(scheduler_t* sched);

/**
 * @brief Schedule a coroutine for execution on a worker.
 *
 * Thread-safe: can be called from any thread (loop, dynpool, worker).
 * Pushes to the global run queue and wakes a worker.
 *
 * @param sched  Scheduler handle.
 * @param co     Coroutine to schedule.
 */
extern void scheduler_schedule(scheduler_t* sched, mco_coro* co);

/**
 * @brief Spawn a new coroutine on the scheduler.
 *
 * Creates a coroutine and schedules it. If called from a worker thread,
 * pushes to local deque; otherwise pushes to inject queue.
 *
 * @param sched  Scheduler handle.
 * @param fn     Coroutine entry function.
 * @param arg    Opaque argument.
 */
extern void scheduler_spawn(
    scheduler_t* sched, void (*fn)(void*), void* arg);

/**
 * @brief Signal the scheduler to shut down.
 *
 * Wakes all workers. They will drain local deques before exiting.
 *
 * @param sched  Scheduler handle.
 */
extern void scheduler_shutdown(scheduler_t* sched);

/**
 * @brief Attach a shared poller to the scheduler.
 *
 * Workers will poll for IO events when idle. The callback is invoked
 * on the worker thread for each ready event.
 *
 * @param sched   Scheduler handle.
 * @param poller  Platform poller handle.
 * @param cb      Callback for ready events.
 */
extern void scheduler_set_poller(
    scheduler_t* sched,
    platform_poller_sq_t* poller,
    scheduler_poll_fn_t cb);

/** @brief Opaque timer manager owned by the scheduler. */
typedef struct sched_timer_mgr_s sched_timer_mgr_t;

/** @brief Callback type for scheduler_post() deferred execution. */
typedef void (*scheduler_post_fn_t)(void* ud);

/**
 * @brief Post a deferred callback to the scheduler.
 *
 * Thread-safe. The callback will be invoked on a worker thread
 * during the next poll/timer processing pass.
 *
 * @param sched  Scheduler handle.
 * @param cb     Callback function.
 * @param ud     User data.
 *
 * @return 0 on success, -1 on failure.
 */
extern int scheduler_post(
    scheduler_t* sched, scheduler_post_fn_t cb, void* ud);

/**
 * @brief Get the scheduler's timer manager.
 *
 * @param sched  Scheduler handle.
 *
 * @return Timer manager handle.
 */
extern sched_timer_mgr_t* scheduler_get_timer_mgr(scheduler_t* sched);
