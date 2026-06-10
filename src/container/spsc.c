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

#include "container/spsc.h"

#include <stdlib.h>

static inline bool _spsc_is_pow2(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

int spsc_init(spsc_t* q, size_t cap) {
    if (!q || !_spsc_is_pow2(cap)) {
        return -1;
    }
    void** slots = (void**)calloc(cap, sizeof(void*));
    if (!slots) {
        return -1;
    }
    q->slots = slots;
    q->mask  = cap - 1;
    atomic_store_explicit(&q->wpos, 0, memory_order_relaxed);
    atomic_store_explicit(&q->rpos, 0, memory_order_relaxed);
    return 0;
}

void spsc_deinit(spsc_t* q) {
    if (!q) {
        return;
    }
    free(q->slots);
    q->slots = NULL;
}

int spsc_push(spsc_t* q, void* elem) {
    /* NULL is the "empty" marker returned by pop; reject it so a queued
     * element is never mistaken for an empty ring. */
    if (elem == NULL) {
        return -1;
    }

    /**
     * Producer owns wpos, so a relaxed load of our own index is enough.
     * rpos is published by the consumer; acquire it so the consumer's
     * read of a slot happens-before we overwrite that slot. Full when
     * the ring already holds cap elements.
     */
    size_t w = atomic_load_explicit(&q->wpos, memory_order_relaxed);
    size_t r = atomic_load_explicit(&q->rpos, memory_order_acquire);
    if (w - r > q->mask) {
        return -1;
    }

    q->slots[w & q->mask] = elem;

    /**
     * Release so a consumer that acquires this wpos also sees the slot
     * written above; this single store publishes the element, so pop
     * never observes a stale slot (no false empty).
     */
    atomic_store_explicit(&q->wpos, w + 1, memory_order_release);
    return 0;
}

void* spsc_pop(spsc_t* q) {
    /**
     * Consumer owns rpos (relaxed self-load). Acquire wpos to pair with
     * the producer's release and observe the published slot.
     */
    size_t r = atomic_load_explicit(&q->rpos, memory_order_relaxed);
    size_t w = atomic_load_explicit(&q->wpos, memory_order_acquire);
    if (w == r) {
        return NULL;
    }

    void* elem = q->slots[r & q->mask];

    /* Release so the producer's acquire of rpos sees the slot as free. */
    atomic_store_explicit(&q->rpos, r + 1, memory_order_release);
    return elem;
}

size_t spsc_len(const spsc_t* q) {
    size_t w = atomic_load_explicit(&q->wpos, memory_order_acquire);
    size_t r = atomic_load_explicit(&q->rpos, memory_order_acquire);
    return w - r;
}

size_t spsc_cap(const spsc_t* q) {
    return q->mask + 1;
}

bool spsc_empty(const spsc_t* q) {
    return spsc_len(q) == 0;
}

bool spsc_full(const spsc_t* q) {
    return spsc_len(q) > q->mask;
}
