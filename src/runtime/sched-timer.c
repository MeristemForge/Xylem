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

#include "sched-timer.h"
#include "runtime.h"

#include "xylem/xylem-utils.h"

#include "container/heap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <threads.h>

struct sched_timer_s {
    heap_node_t        heap_node;
    sched_timer_mgr_t* mgr;
    sched_timer_fn_t   cb;
    void*              ud;
    uint64_t           timeout;
    uint64_t           repeat;
    bool               active;
};

struct sched_timer_mgr_s {
    heap_t timers;
    mtx_t  lock;
};

static int _sched_timer_cmp(
    const heap_node_t* a, const heap_node_t* b) {
    const sched_timer_t* ta =
        heap_entry(a, sched_timer_t, heap_node);
    const sched_timer_t* tb =
        heap_entry(b, sched_timer_t, heap_node);
    if (ta->timeout < tb->timeout) {
        return -1;
    }
    if (ta->timeout > tb->timeout) {
        return 1;
    }
    return 0;
}

sched_timer_mgr_t* sched_timer_mgr_create(void) {
    sched_timer_mgr_t* mgr =
        (sched_timer_mgr_t*)calloc(1, sizeof(*mgr));
    if (!mgr) {
        return NULL;
    }
    heap_init(&mgr->timers, _sched_timer_cmp);
    mtx_init(&mgr->lock, mtx_plain);
    return mgr;
}

void sched_timer_mgr_destroy(sched_timer_mgr_t* mgr) {
    if (!mgr) {
        return;
    }
    mtx_destroy(&mgr->lock);
    free(mgr);
}

int sched_timer_mgr_process(sched_timer_mgr_t* mgr, uint64_t now_ms) {
    for (;;) {
        sched_timer_t* timer = NULL;

        mtx_lock(&mgr->lock);
        heap_node_t* root = heap_peek(&mgr->timers);
        if (root) {
            sched_timer_t* t =
                heap_entry(root, sched_timer_t, heap_node);
            if (t->timeout <= now_ms) {
                heap_dequeue(&mgr->timers);
                if (t->repeat > 0) {
                    t->timeout = now_ms + t->repeat;
                    heap_insert(&mgr->timers, &t->heap_node);
                } else {
                    t->active = false;
                }
                timer = t;
            }
        }
        mtx_unlock(&mgr->lock);

        if (!timer) {
            break;
        }
        timer->cb(timer, timer->ud);
    }

    return sched_timer_mgr_next_timeout(mgr, now_ms);
}

int sched_timer_mgr_next_timeout(
    sched_timer_mgr_t* mgr, uint64_t now_ms) {
    mtx_lock(&mgr->lock);
    heap_node_t* root = heap_peek(&mgr->timers);
    if (!root) {
        mtx_unlock(&mgr->lock);
        return -1;
    }
    sched_timer_t* t =
        heap_entry(root, sched_timer_t, heap_node);
    int timeout;
    if (t->timeout <= now_ms) {
        timeout = 0;
    } else {
        uint64_t diff = t->timeout - now_ms;
        timeout = (diff > INT32_MAX) ? INT32_MAX : (int)diff;
    }
    mtx_unlock(&mgr->lock);
    return timeout;
}

sched_timer_t* sched_timer_create(sched_timer_mgr_t* mgr) {
    sched_timer_t* t = (sched_timer_t*)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->mgr = mgr;
    return t;
}

void sched_timer_destroy(sched_timer_t* timer) {
    if (!timer) {
        return;
    }
    if (timer->active) {
        sched_timer_stop(timer);
    }
    free(timer);
}

void sched_timer_start(
    sched_timer_t*   timer,
    sched_timer_fn_t cb,
    void*            ud,
    uint64_t         timeout_ms,
    uint64_t         repeat_ms) {
    sched_timer_mgr_t* mgr = timer->mgr;
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    mtx_lock(&mgr->lock);
    if (timer->active) {
        heap_remove(&mgr->timers, &timer->heap_node);
    }
    timer->cb      = cb;
    timer->ud      = ud;
    timer->timeout = now + timeout_ms;
    timer->repeat  = repeat_ms;
    timer->active  = true;
    heap_insert(&mgr->timers, &timer->heap_node);
    mtx_unlock(&mgr->lock);

    /* Wake a polling worker so it recalculates its epoll_wait timeout. */
    scheduler_wake(runtime_get_scheduler());
}

void sched_timer_stop(sched_timer_t* timer) {
    if (!timer->active) {
        return;
    }
    sched_timer_mgr_t* mgr = timer->mgr;
    mtx_lock(&mgr->lock);
    if (timer->active) {
        heap_remove(&mgr->timers, &timer->heap_node);
        timer->active = false;
    }
    mtx_unlock(&mgr->lock);
}
