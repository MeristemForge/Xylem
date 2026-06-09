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

#include "wsdeque.h"

#include <stdatomic.h>
#include <stdlib.h>

struct wsdeque_s {
    _Atomic int64_t bottom;
    _Atomic int64_t top;
    mco_coro**      coros;
    int64_t         mask;
};

wsdeque_t* wsdeque_create(uint32_t cap) {
    if (cap == 0 || (cap & (cap - 1)) != 0) {
        return NULL;
    }
    wsdeque_t* dq = (wsdeque_t*)calloc(1, sizeof(wsdeque_t));
    if (!dq) {
        return NULL;
    }

    dq->coros = (mco_coro**)calloc((size_t)cap, sizeof(mco_coro*));
    if (!dq->coros) {
        free(dq);
        return NULL;
    }

    dq->mask = (int64_t)cap - 1;
    atomic_store_explicit(&dq->bottom, 0, memory_order_relaxed);
    atomic_store_explicit(&dq->top, 0, memory_order_relaxed);
    return dq;
}

int32_t wsdeque_remaining(wsdeque_t* dq) {
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&dq->top, memory_order_relaxed);
    int64_t cap = dq->mask + 1;
    int64_t rem = cap - (b - t);
    return (int32_t)(rem > 0 ? rem : 0);
}

void wsdeque_destroy(wsdeque_t* dq) {
    if (!dq) {
        return;
    }
    free(dq->coros);
    free(dq);
}

int wsdeque_push(wsdeque_t* dq, mco_coro* co) {
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&dq->top, memory_order_acquire);

    if (b - t >= dq->mask + 1) {
        return -1;
    }

    dq->coros[b & dq->mask] = co;
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_release);
    return 0;
}

int32_t wsdeque_pop_half(wsdeque_t* dq, mco_coro** out, int32_t cap) {
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&dq->top, memory_order_acquire);

    int64_t size = b - t;
    if (size <= 0) {
        return 0;
    }

    int64_t half = size / 2;
    if (half <= 0) {
        half = 1;
    }
    if (half > (int64_t)cap) {
        half = (int64_t)cap;
    }

    for (int64_t i = 0; i < half; i++) {
        out[i] = dq->coros[(t + i) & dq->mask];
    }

    if (!atomic_compare_exchange_strong_explicit(
            &dq->top, &t, t + half,
            memory_order_seq_cst, memory_order_relaxed)) {
        /* A stealer raced us. Retry with updated top. */
        t = atomic_load_explicit(&dq->top, memory_order_acquire);
        size = b - t;
        if (size <= 0) {
            return 0;
        }
        half = size / 2;
        if (half <= 0) {
            half = 1;
        }
        if (half > (int64_t)cap) {
            half = (int64_t)cap;
        }
        for (int64_t i = 0; i < half; i++) {
            out[i] = dq->coros[(t + i) & dq->mask];
        }
        if (!atomic_compare_exchange_strong_explicit(
                &dq->top, &t, t + half,
                memory_order_seq_cst, memory_order_relaxed)) {
            return 0;
        }
    }

    return (int32_t)half;
}

mco_coro* wsdeque_pop(wsdeque_t* dq) {
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&dq->bottom, b, memory_order_seq_cst);

    /**
     * Chase-Lev take(): the bottom store and this top load form a
     * StoreLoad pair that must not reorder, or this pop and a concurrent
     * steal could both claim the last element (the same coroutine would
     * then run on two workers). release/acquire never orders
     * Store-before-Load; both ends must participate in the single total
     * order, so the load is seq_cst (matching the seq_cst bottom store
     * above and the seq_cst top access on the steal side).
     */
    int64_t t = atomic_load_explicit(&dq->top, memory_order_seq_cst);

    if (t > b) {
        atomic_store_explicit(&dq->bottom, t, memory_order_relaxed);
        return NULL;
    }

    mco_coro* co = dq->coros[b & dq->mask];

    if (t == b) {
        if (!atomic_compare_exchange_strong_explicit(
                &dq->top, &t, t + 1,
                memory_order_seq_cst, memory_order_relaxed)) {
            co = NULL;
        }
        atomic_store_explicit(&dq->bottom, b + 1, memory_order_relaxed);
    }

    return co;
}

int32_t wsdeque_steal_half(wsdeque_t* dq, mco_coro** out, int32_t cap) {
    if (cap <= 0) {
        return 0;
    }

    int64_t t = atomic_load_explicit(&dq->top, memory_order_seq_cst);
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);

    int64_t size = b - t;
    if (size <= 0) {
        return 0;
    }

    int64_t target = (size + 1) / 2;
    if (target > (int64_t)cap) {
        target = (int64_t)cap;
    }

    int32_t n = 0;
    while (n < (int32_t)target) {
        t = atomic_load_explicit(&dq->top, memory_order_seq_cst);
        b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
        if (b - t <= 0) {
            break;
        }

        mco_coro* co = dq->coros[t & dq->mask];

        if (!atomic_compare_exchange_strong_explicit(
                &dq->top, &t, t + 1,
                memory_order_seq_cst, memory_order_relaxed)) {
            break;
        }
        out[n++] = co;
    }

    return n;
}
