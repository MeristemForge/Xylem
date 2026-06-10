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

#include "sync/waiter.h"

#include "runtime/runtime.h"

#include "thrds.h"

/* Per-thread wake semaphore (TLS, reused for the thread's lifetime).
 * Private to this module: callers reach it through waiter_init. */

static tss_t     _waiter_tls_key;
static once_flag _waiter_tls_once = ONCE_FLAG_INIT;

static void _waiter_tls_dtor(void* p) {
    if (p) {
        platform_sem_destroy((platform_sem_t*)p);
    }
}

static void _waiter_tls_init(void) {
    tss_create(&_waiter_tls_key, _waiter_tls_dtor);
}

static platform_sem_t* _waiter_thread_sem(void) {
    call_once(&_waiter_tls_once, _waiter_tls_init);

    platform_sem_t* sem = (platform_sem_t*)tss_get(_waiter_tls_key);
    if (!sem) {
        sem = platform_sem_create(0);
        if (sem) {
            tss_set(_waiter_tls_key, sem);
        }
    }
    return sem;
}

void waiter_init(waiter_t* w) {
    if (mco_running()) {
        /* Coroutine: co is captured later by the park callback, after
         * the yield has actually suspended this coroutine. */
        w->kind  = WAITER_CO;
        w->sched = runtime_get_scheduler();
        return;
    }
    /* External OS thread: tsem may be NULL on OOM (caller must check). */
    w->kind = WAITER_THR;
    w->tsem = _waiter_thread_sem();
}

void waiter_wake(waiter_t w) {
    if (w.kind == WAITER_CO) {
        scheduler_schedule(w.sched, w.co);
    } else {
        platform_sem_post(w.tsem);
    }
}
