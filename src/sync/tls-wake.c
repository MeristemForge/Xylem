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

#include "sync/tls-wake.h"

#include "xylem/xylem-utils.h"

#include "platform/platform-futex.h"

#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdlib.h>

struct tls_wake_s {
    _Atomic uint32_t count; /* token count; the futex word for blocked waits */
};

/* Thread-local; created lazily, freed by the TLS destructor on exit. */

static tss_t     _tls_wake_key;
static once_flag _tls_wake_once = ONCE_FLAG_INIT;

static void _tls_wake_dtor(void* p) {
    free(p);
}

static void _tls_wake_init(void) {
    tss_create(&_tls_wake_key, _tls_wake_dtor);
}

tls_wake_t* tls_wake_self(void) {
    call_once(&_tls_wake_once, _tls_wake_init);

    tls_wake_t* w = (tls_wake_t*)tss_get(_tls_wake_key);
    if (!w) {
        w = (tls_wake_t*)calloc(1, sizeof(tls_wake_t));
        if (w) {
            atomic_init(&w->count, 0);
            tss_set(_tls_wake_key, w);
        }
    }
    return w;
}

/* Take a banked token if one is free: lock-free CAS-decrement. */
static bool _tls_wake_try(tls_wake_t* w) {
    uint32_t c = atomic_load_explicit(&w->count, memory_order_acquire);
    while (c > 0) {
        if (atomic_compare_exchange_weak_explicit(
                &w->count, &c, c - 1,
                memory_order_acquire, memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void tls_wake_wait(tls_wake_t* w) {
    for (;;) {
        if (_tls_wake_try(w)) {
            return;
        }
        /* count == 0: sleep. A post racing the load is caught by the
         * value-compare (the word is no longer 0), so no wake is lost. */
        platform_futex_wait(&w->count, 0);
    }
}

bool tls_wake_timedwait(tls_wake_t* w, uint64_t timeout_ms) {
    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
    for (;;) {
        if (_tls_wake_try(w)) {
            return true;
        }
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now >= deadline) {
            return false;
        }
        platform_futex_timedwait(&w->count, 0, deadline - now);
    }
}

void tls_wake_signal(tls_wake_t* w) {
    atomic_fetch_add_explicit(&w->count, 1, memory_order_release);
    platform_futex_signal(&w->count);
}
