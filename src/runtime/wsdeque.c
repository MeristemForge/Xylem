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
    mco_coro**      buffer;
    int64_t         mask;
};

wsdeque_t* wsdeque_create(uint32_t log2_cap) {
    wsdeque_t* dq = (wsdeque_t*)calloc(1, sizeof(wsdeque_t));
    if (!dq) {
        return NULL;
    }

    int64_t cap = (int64_t)1 << log2_cap;
    dq->buffer = (mco_coro**)calloc((size_t)cap, sizeof(mco_coro*));
    if (!dq->buffer) {
        free(dq);
        return NULL;
    }

    dq->mask = cap - 1;
    atomic_store_explicit(&dq->bottom, 0, memory_order_relaxed);
    atomic_store_explicit(&dq->top, 0, memory_order_relaxed);
    return dq;
}

void wsdeque_destroy(wsdeque_t* dq) {
    if (!dq) {
        return;
    }
    free(dq->buffer);
    free(dq);
}

int wsdeque_push(wsdeque_t* dq, mco_coro* co) {
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&dq->top, memory_order_acquire);

    if (b - t >= dq->mask + 1) {
        return -1;
    }

    dq->buffer[b & dq->mask] = co;
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_release);
    return 0;
}

mco_coro* wsdeque_pop(wsdeque_t* dq) {
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&dq->bottom, b, memory_order_seq_cst);

    int64_t t = atomic_load_explicit(&dq->top, memory_order_acquire);

    if (t > b) {
        atomic_store_explicit(&dq->bottom, t, memory_order_relaxed);
        return NULL;
    }

    mco_coro* co = dq->buffer[b & dq->mask];

    if (t == b) {
        if (!atomic_compare_exchange_strong_explicit(
                &dq->top, &t, t + 1,
                memory_order_seq_cst, memory_order_relaxed)) {
            co = NULL;
        }
        atomic_store_explicit(&dq->bottom, t + 1, memory_order_relaxed);
    }

    return co;
}

mco_coro* wsdeque_steal(wsdeque_t* dq) {
    int64_t t = atomic_load_explicit(&dq->top, memory_order_seq_cst);
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);

    if (t >= b) {
        return NULL;
    }

    mco_coro* co = dq->buffer[t & dq->mask];

    if (!atomic_compare_exchange_strong_explicit(
            &dq->top, &t, t + 1,
            memory_order_seq_cst, memory_order_relaxed)) {
        return NULL;
    }

    return co;
}
