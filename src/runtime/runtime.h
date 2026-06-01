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

#include "runtime/scheduler.h"
#include "runtime/dynpool.h"

#include <stdint.h>

/** @brief Runtime configuration options. */
typedef struct runtime_opts_s {
    int32_t workers;  /*< Thread pool size, 0 for default. */
} runtime_opts_t;

/**
 * @brief Run the global runtime until all coroutines exit.
 *
 * Blocks the calling thread until every spawned coroutine has
 * returned, or runtime_shutdown() is called to force an early exit.
 *
 * @param main_fn  Initial coroutine entry point.
 * @param arg      Opaque argument passed to main_fn.
 * @param opts     Runtime options, NULL for defaults.
 */
extern void runtime_run(
    void (*main_fn)(void*), void* arg, runtime_opts_t* opts);

/**
 * @brief Force the runtime to shut down immediately.
 *
 * Thread-safe. Unblocks runtime_run() without waiting for coroutines
 * to finish naturally.
 */
extern void runtime_shutdown(void);

/**
 * @brief Spawn a new coroutine. Thread-safe.
 *
 * @param fn   Coroutine entry function.
 * @param arg  Argument passed to fn.
 */
extern void runtime_spawn(void (*fn)(void*), void* arg);

/**
 * @brief Suspend the current coroutine for a duration.
 *
 * Must be called from inside a coroutine running on the scheduler.
 *
 * @param ms  Milliseconds to sleep.
 */
extern void runtime_sleep(uint64_t ms);

/**
 * @brief Execute a blocking function on the thread pool.
 *
 * Submits @p fn to the thread pool and suspends the calling
 * coroutine until execution completes. The caller resumes on a
 * scheduler worker thread.
 *
 * @param fn   Function to execute on a worker thread.
 * @param arg  Argument passed to @p fn.
 *
 * @return 0 on success, -1 on failure.
 */
extern int runtime_submit(void (*fn)(void*), void* arg);

/** @brief Get the global scheduler instance. */
extern scheduler_t* runtime_get_scheduler(void);

/** @brief Get the global I/O poller instance. */
extern platform_poller_sq_t* runtime_get_poller(void);

/** @brief Get the global dynamic thread pool instance. */
extern dynpool_t* runtime_get_dynpool(void);
