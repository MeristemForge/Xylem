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

typedef struct xylem_runtime_opts_s {
    int32_t workers;  /*< Thread pool size, 0 for default. */
} xylem_runtime_opts_t;

/**
 * @brief Start the global runtime and block until it exits.
 *
 * The runtime exits when all active handles are closed or
 * xylem_runtime_stop() is called.
 *
 * @param main_fn  Initial coroutine entry point.
 * @param arg      Argument passed to main_fn.
 * @param opts     Runtime options, NULL for defaults.
 */
extern void xylem_runtime_start(void (*main_fn)(void*), void* arg,
                                xylem_runtime_opts_t* opts);

/**
 * @brief Stop the global runtime.
 *
 * Thread-safe.
 */
extern void xylem_runtime_stop(void);

/**
 * @brief Spawn a new coroutine.
 *
 * Thread-safe.
 *
 * @param fn   Coroutine entry function.
 * @param arg  Argument passed to fn.
 */
extern void xylem_runtime_spawn(void (*fn)(void*), void* arg);

/**
 * @brief Suspend the current coroutine for a duration.
 *
 * @param ms  Milliseconds to sleep.
 */
extern void xylem_runtime_sleep(uint64_t ms);
