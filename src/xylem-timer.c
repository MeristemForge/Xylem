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

#include "xylem/xylem-timer.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stddef.h>

/**
 * The public xylem_timer_t is an opaque wrapper whose single member is the
 * scheduler's scheduler_timer_t. The handle converts to the engine timer via
 * first-member address equivalence (C 6.7.2.1), i.e. the static assert
 * below guarantees (scheduler_timer_t*)pub == &pub->internal.
 */
struct xylem_timer_s {
    scheduler_timer_t internal;
};

_Static_assert(offsetof(struct xylem_timer_s, internal) == 0,
               "scheduler_timer_t must remain the first member of xylem_timer_s");

xylem_timer_t* xylem_timer_after(
    uint64_t delay_ms, xylem_timer_fn_t cb, void* ud) {
    scheduler_t* sched = runtime_get_scheduler();
    if (!sched || !cb) {
        return NULL;
    }
    scheduler_timer_t* t = scheduler_timer_create(sched);
    if (!t) {
        return NULL;
    }
    scheduler_timer_set_spawn(t, true);
    scheduler_timer_start(t, (scheduler_timer_fn_t)cb, ud, delay_ms, 0);
    return (xylem_timer_t*)t;
}

bool xylem_timer_cancel(xylem_timer_t* timer) {
    if (!timer) {
        return false;
    }
    scheduler_timer_t* t = &timer->internal;
    bool stopped = scheduler_timer_stop(t);
    scheduler_timer_destroy(t);
    return stopped;
}

bool xylem_timer_reset(xylem_timer_t* timer, uint64_t delay_ms) {
    if (!timer) {
        return false;
    }
    return scheduler_timer_reset(&timer->internal, delay_ms);
}
