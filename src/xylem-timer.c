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

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

struct xylem_timer_s {
    scheduler_timer_t* internal;
    xylem_timer_fn_t   cb;
    void*              ud;
    bool               repeat;
    _Atomic int32_t    refcnt;
};

static void _timer_ref(void* ud) {
    xylem_timer_t* timer = (xylem_timer_t*)ud;
    atomic_fetch_add(&timer->refcnt, 1);
}

static void _timer_unref(void* ud) {
    xylem_timer_t* timer = (xylem_timer_t*)ud;
    if (atomic_fetch_sub(&timer->refcnt, 1)
        == 1) {
        free(timer);
    }
}

static void _timer_fire_cb(scheduler_timer_t* internal, void* ud) {
    (void)internal;
    xylem_timer_t* timer = (xylem_timer_t*)ud;
    timer->cb(timer, timer->ud);
}

static xylem_timer_t* _timer_create(
    uint64_t         delay_ms,
    uint64_t         repeat_ms,
    xylem_timer_fn_t cb,
    void*            ud) {
    scheduler_t* sched = runtime_get_scheduler();
    if (!sched || !cb) {
        return NULL;
    }
    xylem_timer_t* timer =
        (xylem_timer_t*)calloc(1, sizeof(xylem_timer_t));
    if (!timer) {
        return NULL;
    }

    scheduler_timer_t* t = scheduler_timer_create(sched);
    if (!t) {
        free(timer);
        return NULL;
    }

    timer->internal = t;
    timer->cb       = cb;
    timer->ud       = ud;
    timer->repeat   = (repeat_ms != 0);
    atomic_init(&timer->refcnt, 1);

    scheduler_timer_set_spawn(t, true);
    scheduler_timer_set_ud_guard(t, _timer_ref, _timer_unref);
    /* start never fails for a valid timer; timer just created above. */
    scheduler_timer_start(t, _timer_fire_cb, timer, delay_ms, repeat_ms);
    return timer;
}

xylem_timer_t* xylem_timer_after(
    uint64_t delay_ms, xylem_timer_fn_t cb, void* ud) {
    return _timer_create(delay_ms, 0, cb, ud);
}

xylem_timer_t* xylem_timer_every(
    uint64_t interval_ms, xylem_timer_fn_t cb, void* ud) {
    if (interval_ms == 0) {
        return NULL;
    }
    return _timer_create(interval_ms, interval_ms, cb, ud);
}

bool xylem_timer_cancel(xylem_timer_t* timer) {
    if (!timer) {
        return false;
    }
    scheduler_timer_t* t = timer->internal;
    bool stopped = scheduler_timer_stop(t);
    scheduler_timer_destroy(t);
    _timer_unref(timer);
    return stopped;
}

bool xylem_timer_reset(xylem_timer_t* timer, uint64_t delay_ms) {
    if (!timer) {
        return false;
    }
    if (timer->repeat && delay_ms == 0) {
        return false;
    }
    return scheduler_timer_reset(timer->internal, delay_ms);
}
