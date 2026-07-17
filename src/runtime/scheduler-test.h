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

typedef void (*scheduler_test_park_hook_fn_t)(void* arg);

typedef struct scheduler_test_park_hook_s {
    scheduler_test_park_hook_fn_t fn;
    void*                         arg;
    mco_coro*                     target;
} scheduler_test_park_hook_t;

/**
 * @brief Install a one-shot scheduler park checkpoint hook.
 *
 * @param sched  Scheduler handle.
 * @param hook   One-shot hook to publish, or NULL as described below.
 *
 * @note Hook fields and arg are immutable after publication. Hook and arg must
 *       remain valid until consumed or the scheduler workers are quiescent.
 * @note Concurrent replacement or cancellation while scheduler workers are
 *       running is unsupported. Passing NULL clears an unobserved hook only
 *       when scheduler workers are quiescent.
 */
extern void scheduler_test_set_park_hook(
    scheduler_t* sched,
    scheduler_test_park_hook_t* hook);
