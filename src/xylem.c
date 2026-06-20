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

/**
 * Thin public facade over the internal runtime. xylem_* forwards
 * one-to-one onto runtime_* and never introduces its own state:
 * keeping the bodies trivial lets us evolve the internal API
 * (runtime_*, scheduler_*, iowait_*) without breaking consumers
 * of xylem.h.
 *
 * The one deliberate exception is xylem_sleep, which is
 * context-adaptive: it parks the coroutine via runtime_sleep when
 * called on a scheduler worker, and falls back to thrd_sleep on a
 * plain OS thread. The branch lives here, in the facade, so the
 * runtime layer stays purely coroutine-scoped and never depends on
 * the OS-thread sleep primitive.
 *
 * xylem_await, by contrast, is coroutine-only: its sole purpose is to
 * keep a scheduler worker free while blocking work runs, which only
 * makes sense on a worker. runtime_submit aborts if called off a
 * coroutine.
 */

#include "xylem.h"

#include "runtime/runtime.h"
#include "xylem/xylem-threads.h"

#include "runtime/minicoro/minicoro.h"

#include <time.h>

void xylem_run(
    void (*main_fn)(void*), void* arg, xylem_opts_t* opts) {
    runtime_opts_t rt_opts = {
        .workers = opts ? opts->workers : 0,
        .coro_stack_size = opts ? opts->coro_stack_size : 0,
    };
    runtime_run(main_fn, arg, &rt_opts);
}

void xylem_shutdown(void) {
    runtime_shutdown();
}

void xylem_spawn(void (*fn)(void*), void* arg) {
    (void)runtime_spawn(fn, arg);
}

void xylem_sleep(uint64_t ms) {
    if (mco_running()) {
        runtime_sleep(ms);
        return;
    }

    /**
     * Plain OS thread: block the thread for the requested duration.
     * nanosleep/thrd_sleep may return early on a signal (EINTR); keep
     * sleeping for the remainder so the full duration is observed.
     */
    struct timespec req = {
        .tv_sec  = (time_t)(ms / 1000u),
        .tv_nsec = (long)((ms % 1000u) * 1000000u),
    };
    struct timespec rem;
    while (thrd_sleep(&req, &rem) == -1) {
        req = rem;
    }
}

int xylem_await(void (*fn)(void*), void* arg) {
    return runtime_submit(fn, arg);
}
