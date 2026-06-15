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

#include "wsq.h"

#include <stdatomic.h>
#include <stdlib.h>

/**
 * FIFO ring with monotonically increasing head/tail counters; the slot index
 * is the counter masked to the (power-of-two) capacity. tail is advanced only
 * by the owner's push (single producer); head is advanced by the owner's pop
 * and by thieves, both via compare-and-swap. A slot at index h is never
 * overwritten until head passes it, because push refuses to advance tail past
 * head + cap, so the read-then-CAS on the consumer side is safe.
 */
struct wsq_s {
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    void**           slots;
    uint64_t         mask;
};

/**
 * Take up to half of the queued items from the head into out (CAS on head).
 * Shared by the owner overflow drain and by thieves; both contend on head.
 */
static int32_t _wsq_grab_half(wsq_t* q, void** out, int32_t cap) {
    if (cap <= 0) {
        return 0;
    }

    for (;;) {
        uint64_t h = atomic_load_explicit(&q->head, memory_order_acquire);
        uint64_t t = atomic_load_explicit(&q->tail, memory_order_acquire);

        if (t <= h) {
            return 0;
        }

        uint64_t size = t - h;
        uint64_t n    = (size + 1) / 2;
        if (n > (uint64_t)cap) {
            n = (uint64_t)cap;
        }

        for (uint64_t i = 0; i < n; i++) {
            out[i] = q->slots[(h + i) & q->mask];
        }

        if (atomic_compare_exchange_weak_explicit(
                &q->head, &h, h + n,
                memory_order_seq_cst, memory_order_relaxed)) {
            return (int32_t)n;
        }
        /* A concurrent consumer advanced the head; reload and retry. */
    }
}

wsq_t* wsq_create(uint32_t cap) {
    if (cap == 0 || (cap & (cap - 1)) != 0) {
        return NULL;
    }

    wsq_t* q = (wsq_t*)calloc(1, sizeof(wsq_t));
    if (!q) {
        return NULL;
    }

    q->slots = (void**)calloc((size_t)cap, sizeof(void*));
    if (!q->slots) {
        free(q);
        return NULL;
    }

    q->mask = (uint64_t)cap - 1;
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
    return q;
}

void wsq_destroy(wsq_t* q) {
    if (!q) {
        return;
    }
    free(q->slots);
    free(q);
}

int32_t wsq_remaining(wsq_t* q) {
    uint64_t t   = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint64_t h   = atomic_load_explicit(&q->head, memory_order_acquire);
    uint64_t cap = q->mask + 1;
    uint64_t used = t - h;
    return (int32_t)(used < cap ? cap - used : 0);
}

int wsq_push(wsq_t* q, void* elem) {
    uint64_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint64_t h = atomic_load_explicit(&q->head, memory_order_acquire);

    if (t - h >= q->mask + 1) {
        return -1;
    }

    q->slots[t & q->mask] = elem;
    atomic_store_explicit(&q->tail, t + 1, memory_order_release);
    return 0;
}

void* wsq_pop(wsq_t* q) {
    for (;;) {
        uint64_t h = atomic_load_explicit(&q->head, memory_order_acquire);
        uint64_t t = atomic_load_explicit(&q->tail, memory_order_acquire);

        if (h == t) {
            return NULL;
        }

        void* elem = q->slots[h & q->mask];

        /**
         * Read the slot before claiming it. The CAS both publishes the
         * claim and rejects the attempt if a thief took this item first;
         * on failure h is reloaded by the CAS, so the retry sees the new
         * head. seq_cst matches the thief-side head CAS.
         */
        if (atomic_compare_exchange_weak_explicit(
                &q->head, &h, h + 1,
                memory_order_seq_cst, memory_order_relaxed)) {
            return elem;
        }
    }
}

int32_t wsq_pop_half(wsq_t* q, void** out, int32_t cap) {
    return _wsq_grab_half(q, out, cap);
}

int32_t wsq_steal_half(wsq_t* q, void** out, int32_t cap) {
    return _wsq_grab_half(q, out, cap);
}
