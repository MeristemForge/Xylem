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

struct wsq_s {
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    void**           slots;
    uint32_t         mask;
};

static inline uint32_t _wsq_len(uint32_t head, uint32_t tail) {
    return tail - head;
}

static inline uint32_t _wsq_cap(wsq_t* q) {
    return q->mask + 1;
}

static int32_t _wsq_grab_half(wsq_t* q, void** out, int32_t cap) {
    if (cap <= 0) {
        return 0;
    }

    for (;;) {
        uint32_t h = atomic_load(&q->head);
        uint32_t t = atomic_load(&q->tail);

        uint32_t n = _wsq_len(h, t);
        n = n - n / 2;
        if (n == 0) {
            return 0;
        }
        if (n > (uint32_t)cap) {
            n = (uint32_t)cap;
        }

        for (uint32_t i = 0; i < n; i++) {
            out[i] = q->slots[(h + i) & q->mask];
        }

        if (atomic_compare_exchange_weak(&q->head, &h, h + n)) {
            return (int32_t)n;
        }
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

    q->mask = cap - 1;
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
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
    uint32_t h    = atomic_load(&q->head);
    uint32_t t    = atomic_load(&q->tail);
    uint32_t cap  = _wsq_cap(q);
    uint32_t used = _wsq_len(h, t);
    return (int32_t)(used < cap ? cap - used : 0);
}

int wsq_push(wsq_t* q, void* elem) {
    uint32_t t = atomic_load(&q->tail);
    uint32_t h = atomic_load(&q->head);

    if (_wsq_len(h, t) >= _wsq_cap(q)) {
        return -1;
    }

    q->slots[t & q->mask] = elem;
    atomic_store(&q->tail, t + 1);
    return 0;
}

void* wsq_pop(wsq_t* q) {
    for (;;) {
        uint32_t head = atomic_load(&q->head);
        uint32_t tail  = atomic_load(&q->tail);

        if (head == tail) {
            return NULL;
        }

        void* elem = q->slots[head & q->mask];

        if (atomic_compare_exchange_weak(&q->head, &head, head + 1)) {
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
