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

#include "xylem/xylem-logger.h"

#include "arena.h"
#include "sync/spin.h"

#include <stdlib.h>

#define COPOOL_BATCH_SIZE (COPOOL_CACHE_CAP / 2)

struct copool_s {
    spin_t  lock;
    void**  slots;
    int32_t count;
    int32_t cap;
    size_t  slot_size;
    arena_t* arena;
};

static int _copool_shared_take(
    copool_t* pool,
    void** slots,
    int count) {
    spin_lock(&pool->lock);
    int take_count = pool->count;
    if (take_count > count) {
        take_count = count;
    }
    for (int i = 0; i < take_count; i++) {
        slots[i] = pool->slots[--pool->count];
    }
    spin_unlock(&pool->lock);
    return take_count;
}

static int _copool_shared_put(
    copool_t* pool,
    void** slots,
    int count) {
    spin_lock(&pool->lock);
    int put_count = pool->cap - pool->count;
    if (put_count > count) {
        put_count = count;
    }
    for (int i = 0; i < put_count; i++) {
        pool->slots[pool->count++] = slots[i];
    }
    spin_unlock(&pool->lock);
    return put_count;
}

copool_t* copool_create(size_t slot_size, int32_t shared_cap) {
    if (slot_size == 0 || shared_cap < 0) {
        return NULL;
    }

    arena_t* arena = arena_create(slot_size);
    if (!arena) {
        return NULL;
    }

    copool_t* pool = (copool_t*)calloc(1, sizeof(copool_t));
    if (!pool) {
        arena_destroy(arena);
        return NULL;
    }

    if (shared_cap > 0) {
        pool->slots = (void**)calloc((size_t)shared_cap, sizeof(void*));
        if (!pool->slots) {
            free(pool);
            arena_destroy(arena);
            return NULL;
        }
    }

    spin_init(&pool->lock);
    pool->cap = shared_cap;
    pool->slot_size = slot_size;
    pool->arena = arena;
    return pool;
}

void copool_destroy(copool_t* pool) {
    if (!pool) {
        return;
    }

    free(pool->slots);
    arena_destroy(pool->arena);
    free(pool);
}

void* copool_alloc(
    copool_t* pool,
    copool_cache_t* cache,
    size_t size) {
    if (!pool || size == 0 || size > pool->slot_size) {
        return NULL;
    }

    if (cache) {
        if (cache->count > 0) {
            return cache->slots[--cache->count];
        }

        cache->count = _copool_shared_take(
            pool,
            cache->slots,
            COPOOL_BATCH_SIZE);
        if (cache->count == 0) {
            cache->count = arena_alloc(
                pool->arena,
                cache->slots,
                COPOOL_BATCH_SIZE);
        }
        if (cache->count == 0) {
            return NULL;
        }
        return cache->slots[--cache->count];
    }

    void* batch[COPOOL_BATCH_SIZE];
    if (_copool_shared_take(pool, batch, 1) == 1) {
        return batch[0];
    }

    int alloc_count = arena_alloc(
        pool->arena,
        batch,
        COPOOL_BATCH_SIZE);
    if (alloc_count == 0) {
        return NULL;
    }

    int put_count = _copool_shared_put(pool, &batch[1], alloc_count - 1);
    int free_count = alloc_count - 1 - put_count;
    if (free_count > 0) {
        arena_free(
            pool->arena,
            &batch[1 + put_count],
            free_count);
    }
    return batch[0];
}

void copool_free(
    copool_t* pool,
    copool_cache_t* cache,
    void* ptr,
    size_t size) {
    if (!pool || !ptr) {
        return;
    }
    if (size == 0 || size > pool->slot_size) {
        xylem_loge(
            "<copool> invalid free size size=%zu slot_size=%zu",
            size,
            pool->slot_size);
        abort();
    }

    if (cache) {
        if (cache->count < COPOOL_CACHE_CAP) {
            cache->slots[cache->count++] = ptr;
            return;
        }

        void* batch[COPOOL_BATCH_SIZE];
        for (int i = 0; i < COPOOL_BATCH_SIZE; i++) {
            batch[i] = cache->slots[--cache->count];
        }

        int put_count = _copool_shared_put(
            pool,
            batch,
            COPOOL_BATCH_SIZE);
        if (put_count < COPOOL_BATCH_SIZE) {
            arena_free(
                pool->arena,
                &batch[put_count],
                COPOOL_BATCH_SIZE - put_count);
        }
        cache->slots[cache->count++] = ptr;
        return;
    }

    void* slot = ptr;
    if (_copool_shared_put(pool, &slot, 1) == 0) {
        arena_free(pool->arena, &slot, 1);
    }
}
