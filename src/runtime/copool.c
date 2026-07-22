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

#include "copool.h"

#include "container/list.h"
#include "sync/spin.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct copool_local_s {
    int           count;
    int           capacity;
    copool_slot_t slots[];
};

typedef struct _copool_shared_node_s {
    list_node_t   node;
    copool_slot_t slot;
} _copool_shared_node_t;

struct copool_shared_s {
    list_t slots;
    spin_t lock;
};

static int _copool_slot_valid(const copool_slot_t* slot) {
    return slot->ptr &&
           (slot->state == COPOOL_SLOT_FRESH ||
            slot->state == COPOOL_SLOT_REUSABLE);
}

static void _copool_destroy_entries(list_t* entries) {
    list_node_t* node;
    while ((node = list_head(entries)) != NULL) {
        list_remove(entries, node);
        free(list_entry(node, _copool_shared_node_t, node));
    }
}

copool_local_t* copool_local_create(int capacity) {
    if (capacity == 0) {
        capacity = COPOOL_LOCAL_DEFAULT_CAP;
    }
    if (capacity < 0 ||
        (size_t)capacity > (SIZE_MAX - sizeof(copool_local_t)) /
                               sizeof(copool_slot_t)) {
        return NULL;
    }

    size_t size =
        sizeof(copool_local_t) + (size_t)capacity * sizeof(copool_slot_t);
    copool_local_t* pool = (copool_local_t*)calloc(1, size);
    if (!pool) {
        return NULL;
    }
    pool->capacity = capacity;
    return pool;
}

void copool_local_destroy(copool_local_t* pool) {
    free(pool);
}

int copool_local_capacity(const copool_local_t* pool) {
    return pool ? pool->capacity : 0;
}

int copool_local_take(
    copool_local_t* pool,
    copool_slot_t*  slots,
    int             count) {
    if (!pool || !slots || count <= 0) {
        return 0;
    }

    int take_count = pool->count;
    if (take_count > count) {
        take_count = count;
    }
    for (int i = 0; i < take_count; i++) {
        slots[i] = pool->slots[--pool->count];
    }
    return take_count;
}

int copool_local_put(
    copool_local_t*      pool,
    const copool_slot_t* slots,
    int                  count) {
    if (!pool || !slots || count <= 0) {
        return 0;
    }

    int put_count = pool->capacity - pool->count;
    if (put_count > count) {
        put_count = count;
    }
    int i = 0;
    while (i < put_count && _copool_slot_valid(&slots[i])) {
        pool->slots[pool->count++] = slots[i++];
    }
    return i;
}

copool_shared_t* copool_shared_create(void) {
    copool_shared_t* pool =
        (copool_shared_t*)calloc(1, sizeof(copool_shared_t));
    if (!pool) {
        return NULL;
    }
    list_init(&pool->slots);
    spin_init(&pool->lock);
    return pool;
}

void copool_shared_destroy(copool_shared_t* pool) {
    if (!pool) {
        return;
    }
    _copool_destroy_entries(&pool->slots);
    free(pool);
}

int copool_shared_take(
    copool_shared_t* pool,
    copool_slot_t*   slots,
    int              count) {
    if (!pool || !slots || count <= 0) {
        return 0;
    }

    list_t removed;
    list_init(&removed);

    spin_lock(&pool->lock);
    int take_count = 0;
    while (take_count < count) {
        list_node_t* node = list_tail(&pool->slots);
        if (!node) {
            break;
        }
        _copool_shared_node_t* entry =
            list_entry(node, _copool_shared_node_t, node);
        list_remove(&pool->slots, node);
        slots[take_count++] = entry->slot;
        list_insert_tail(&removed, node);
    }
    spin_unlock(&pool->lock);

    _copool_destroy_entries(&removed);
    return take_count;
}

int copool_shared_put(
    copool_shared_t*     pool,
    const copool_slot_t* slots,
    int                  count) {
    if (!pool || !slots || count <= 0) {
        return 0;
    }

    list_t pending;
    list_init(&pending);

    int put_count = 0;
    while (put_count < count && _copool_slot_valid(&slots[put_count])) {
        _copool_shared_node_t* entry =
            (_copool_shared_node_t*)calloc(1, sizeof(_copool_shared_node_t));
        if (!entry) {
            break;
        }
        entry->slot = slots[put_count++];
        list_insert_tail(&pending, &entry->node);
    }

    spin_lock(&pool->lock);
    list_node_t* node;
    while ((node = list_head(&pending)) != NULL) {
        list_remove(&pending, node);
        list_insert_tail(&pool->slots, node);
    }
    spin_unlock(&pool->lock);
    return put_count;
}
