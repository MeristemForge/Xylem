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

typedef struct mco_coro      mco_coro;
typedef struct scheduler_s   scheduler_t;
typedef struct sched_timer_s sched_timer_t;

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
 * @brief Callback type for scheduler_post() deferred execution.
 *
 * @param ud  User data passed to scheduler_post().
 */
typedef void (*scheduler_post_fn_t)(void* ud);

/**
 * @brief Timer expiry callback.
 *
 * @param timer  The timer that fired.
 * @param ud     User data from sched_timer_start().
 */
typedef void (*sched_timer_fn_t)(sched_timer_t* timer, void* ud);

typedef struct scheduler_opts_s {
    int32_t  nworkers;  /*< 0 = use CPU count. */
    uint32_t deque_cap; /*< 0 = use default (log2=10, 1024 slots). */
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
 * Signals shutdown, wakes all workers, waits for them to drain
 * their queues, then frees all resources.
 *
 * @param sched  Scheduler to destroy, or NULL (no-op).
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
 * @brief Get the scheduler's poller handle.
 *
 * @param sched  Scheduler handle.
 *
 * @return Platform poller handle.
 */
extern platform_poller_sq_t* scheduler_get_poller(scheduler_t* sched);

/**
 * @brief Create a timer attached to a scheduler.
 *
 * @param sched  Scheduler handle.
 *
 * @return Timer handle, or NULL on failure.
 */
extern sched_timer_t* sched_timer_create(scheduler_t* sched);

/**
 * @brief Destroy a timer. Stops it first if active.
 *
 * @param timer  Timer handle, or NULL (no-op).
 */
extern void sched_timer_destroy(sched_timer_t* timer);

/**
 * @brief Start or restart a timer. Thread-safe.
 *
 * @param timer       Timer handle.
 * @param cb          Callback to invoke on expiry.
 * @param ud          User data for callback.
 * @param timeout_ms  Delay in milliseconds.
 * @param repeat_ms   Repeat interval, 0 for one-shot.
 */
extern void sched_timer_start(
    sched_timer_t*   timer,
    sched_timer_fn_t cb,
    void*            ud,
    uint64_t         timeout_ms,
    uint64_t         repeat_ms);

/**
 * @brief Stop a running timer. Thread-safe. No-op if already stopped.
 *
 * @param timer  Timer handle.
 */
extern void sched_timer_stop(sched_timer_t* timer);

/**
 * @brief Callback invoked when all coroutines have exited.
 *
 * @param ud  User data from scheduler_set_idle_cb().
 */
typedef void (*scheduler_idle_fn_t)(void* ud);

/**
 * @brief Register a callback for when all coroutines have exited.
 *
 * The callback fires once when the alive coroutine count drops to zero.
 * Thread-safe. Only one callback may be registered at a time.
 *
 * @param sched  Scheduler handle.
 * @param cb     Callback, or NULL to clear.
 * @param ud     User data passed to cb.
 */
extern void scheduler_set_idle_cb(
    scheduler_t* sched, scheduler_idle_fn_t cb, void* ud);
