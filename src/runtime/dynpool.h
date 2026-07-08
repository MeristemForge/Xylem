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

#include <stdint.h>

/* Opaque dynamic thread pool handle. */
typedef struct dynpool_s dynpool_t;

/* Configuration for dynpool_create. */
typedef struct dynpool_opts_s {
    int32_t  max_threads;   /* 0 = use default (512) */
    uint64_t idle_timeout;  /* 0 = use default (10000 ms) */
} dynpool_opts_t;

/**
 * @brief Create a dynamic blocking pool.
 *
 * Threads are spawned on demand and exit after idle timeout.
 *
 * @param opts  Configuration options, or NULL for defaults.
 *
 * @return Pool handle, or NULL on failure.
 */
extern dynpool_t* dynpool_create(dynpool_opts_t* opts);

/**
 * @brief Submit a blocking task to the pool.
 *
 * Submission serializes the task queue and worker lifecycle state under the
 * pool mutex. A pool thread will execute the task. Must not race with
 * dynpool_destroy().
 *
 * @param pool     Pool handle.
 * @param routine  Function to execute.
 * @param arg      Opaque argument passed to routine.
 *
 * @return 0 on success, -1 on failure.
 */
extern int dynpool_submit(
    dynpool_t* pool,
    void (*routine)(void*),
    void* arg);

/**
 * @brief Destroy the pool.
 *
 * Signals all workers to exit, waits for running tasks to complete, drops any
 * remaining queued tasks, and frees resources.
 *
 * @note This is a final release operation, not a concurrent stop operation.
 *       The caller must ensure no dynpool_submit() calls are in flight and
 *       must not call this function from a pool worker task.
 *
 * @param pool  Pool handle.
 */
extern void dynpool_destroy(dynpool_t* pool);
