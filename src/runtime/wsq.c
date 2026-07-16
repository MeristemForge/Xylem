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
#include <stdint.h>
#include <stdlib.h>

struct wsq_s {
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    _Atomic(void*)*  slots;
    uint32_t         mask;
};

static inline uint64_t _wsq_len(uint64_t head, uint64_t tail) {
    return tail - head;
}

static inline uint32_t _wsq_cap(wsq_t* q) {
    return q->mask + 1;
}

static int _wsq_grab_half(wsq_t* q, void** elems, int elems_cap) {
    if (elems_cap <= 0) {
        return 0;
    }

    uint64_t head  = atomic_load(&q->head);
    uint64_t limit = (uint64_t)elems_cap;

    for (;;) {
        uint64_t count = _wsq_len(head, atomic_load(&q->tail));
        if (count == 0) {
            return 0;
        }

        count = (count + 1) / 2;
        if (count > limit) {
            count = limit;
        }

        for (uint64_t i = 0; i < count; i++) {
            elems[i] = atomic_load(&q->slots[(head + i) & q->mask]);
        }

        if (atomic_compare_exchange_weak(&q->head, &head, head + count)) {
            return (int)count;
        }
    }
}

wsq_t* wsq_create(int cap) {
    if (cap <= 0 || (cap & (cap - 1)) != 0) {
        return NULL;
    }

    uint32_t capacity = (uint32_t)cap;

    wsq_t* q = (wsq_t*)calloc(1, sizeof(wsq_t));
    if (!q) {
        return NULL;
    }

    q->slots = (_Atomic(void*)*)calloc(
        (size_t)capacity, sizeof(*q->slots));
    if (!q->slots) {
        free(q);
        return NULL;
    }

    q->mask = capacity - 1;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    for (uint32_t i = 0; i < capacity; i++) {
        atomic_init(&q->slots[i], NULL);
    }
    return q;
}

void wsq_destroy(wsq_t* q) {
    if (!q) {
        return;
    }
    free((void*)q->slots);
    free(q);
}

int wsq_remaining(wsq_t* q) {
    uint64_t head     = atomic_load(&q->head);
    uint64_t tail     = atomic_load(&q->tail);
    uint64_t capacity = _wsq_cap(q);
    uint64_t used     = _wsq_len(head, tail);
    return (int)(used < capacity ? capacity - used : 0);
}

int wsq_push(wsq_t* q, void* elem) {
    if (!elem) {
        return -1;
    }

    uint64_t tail = atomic_load(&q->tail);
    uint64_t head = atomic_load(&q->head);

    if (_wsq_len(head, tail) >= _wsq_cap(q)) {
        return -1;
    }

    atomic_store(&q->slots[tail & q->mask], elem);
    atomic_store(&q->tail, tail + 1);
    return 0;
}

void* wsq_pop(wsq_t* q) {
    uint64_t head = atomic_load(&q->head);

    for (;;) {
        if (head == atomic_load(&q->tail)) {
            return NULL;
        }

        void* elem = atomic_load(&q->slots[head & q->mask]);

        if (atomic_compare_exchange_weak(&q->head, &head, head + 1)) {
            return elem;
        }
    }
}

int wsq_pop_half(wsq_t* q, void** elems, int elems_cap) {
    return _wsq_grab_half(q, elems, elems_cap);
}

int wsq_steal_half(wsq_t* q, void** elems, int elems_cap) {
    return _wsq_grab_half(q, elems, elems_cap);
}
