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

#include "sync/thrd-wake.h"

#include "xylem/xylem-utils.h"
#include "xylem/xylem-threads.h"

#include "platform/platform-futex.h"

#include <stdatomic.h>
#include <stdlib.h>

struct thrd_wake_s {
    _Atomic uint32_t count; /* token count; the futex word for blocked waits */
};

/* Thread-local; created lazily, freed by the TLS destructor on exit. */

static tss_t     _thrd_wake_key;
static once_flag _thrd_wake_once = ONCE_FLAG_INIT;
static bool      _thrd_wake_ready;

static void _thrd_wake_dtor(void* p) {
    free(p);
}

static void _thrd_wake_init(void) {
    _thrd_wake_ready =
        (tss_create(&_thrd_wake_key, _thrd_wake_dtor) == thrd_success);
}

thrd_wake_t* thrd_wake_self(void) {
    call_once(&_thrd_wake_once, _thrd_wake_init);
    if (!_thrd_wake_ready) {
        abort();
    }

    thrd_wake_t* w = (thrd_wake_t*)tss_get(_thrd_wake_key);
    if (!w) {
        w = (thrd_wake_t*)calloc(1, sizeof(thrd_wake_t));
        if (!w) {
            abort();
        }
        atomic_init(&w->count, 0);
        if (tss_set(_thrd_wake_key, w) != thrd_success) {
            free(w);
            abort();
        }
    }
    return w;
}

/* Take a banked token if one is free: lock-free CAS-decrement. */
static bool _thrd_wake_try(thrd_wake_t* w) {
    uint32_t c = atomic_load(&w->count);
    while (c > 0) {
        if (atomic_compare_exchange_weak(&w->count, &c, c - 1)) {
            return true;
        }
    }
    return false;
}

void thrd_wake_wait(thrd_wake_t* w) {
    for (;;) {
        if (_thrd_wake_try(w)) {
            return;
        }
        /**
         * count == 0: sleep. A post racing the load is caught by the
         * value-compare (the word is no longer 0), so no wake is lost.
         */
        platform_futex_wait(&w->count, 0);
    }
}

bool thrd_wake_timedwait(thrd_wake_t* w, uint64_t timeout_ms) {
    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
    for (;;) {
        if (_thrd_wake_try(w)) {
            return true;
        }
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now >= deadline) {
            return false;
        }
        platform_futex_timedwait(&w->count, 0, deadline - now);
    }
}

void thrd_wake_signal(thrd_wake_t* w) {
    atomic_fetch_add(&w->count, 1);
    platform_futex_signal(&w->count);
}
