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

xylem_timer_t* xylem_timer_after(
    uint64_t delay_ms, xylem_timer_fn_t cb, void* ud) {
    scheduler_t* sched = runtime_get_scheduler();
    if (!sched || !cb) {
        return NULL;
    }
    sched_timer_t* t = sched_timer_create(sched);
    if (!t) {
        return NULL;
    }
    sched_timer_start(t, cb, ud, delay_ms, 0);
    return t;
}

xylem_timer_t* xylem_timer_every(
    uint64_t period_ms, xylem_timer_fn_t cb, void* ud) {
    if (period_ms == 0) {
        return NULL;
    }
    scheduler_t* sched = runtime_get_scheduler();
    if (!sched || !cb) {
        return NULL;
    }
    sched_timer_t* t = sched_timer_create(sched);
    if (!t) {
        return NULL;
    }
    sched_timer_start(t, cb, ud, period_ms, period_ms);
    return t;
}

bool xylem_timer_cancel(xylem_timer_t* t) {
    if (!t) {
        return false;
    }
    bool stopped = sched_timer_stop(t);
    sched_timer_destroy(t);
    return stopped;
}

bool xylem_timer_reset(xylem_timer_t* t, uint64_t timeout_ms) {
    if (!t) {
        return false;
    }
    return sched_timer_reset(t, timeout_ms);
}
