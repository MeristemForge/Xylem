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

#define COPOOL_LOCAL_CAP 64

typedef struct {
    int32_t count;
    int32_t cap;
    void*   slots[];
} copool_cache_t;

struct copool_s {
    copool_cache_t**  local_pools;
    int32_t           local_pool_count;
    spin_t            shared_lock;
    copool_cache_t*   shared_pool;
    size_t            slot_size;
    copool_slot_ops_t ops;
    arena_t*          arena;
};

static copool_cache_t* _copool_cache_create(int32_t cap) {
    if (cap < 0 || (size_t)cap >
        (SIZE_MAX - sizeof(copool_cache_t)) / sizeof(void*)) {
        return NULL;
    }

    size_t size = sizeof(copool_cache_t) + (size_t)cap * sizeof(void*);
    copool_cache_t* cache = (copool_cache_t*)calloc(1, size);
    if (!cache) {
        return NULL;
    }
    cache->cap = cap;
    return cache;
}

static void _copool_cache_destroy(copool_cache_t* cache) {
    free(cache);
}

static int _copool_cache_take(
    copool_cache_t* cache,
    void** slots,
    int count) {
    int take_count = cache->count;
    if (take_count > count) {
        take_count = count;
    }
    for (int i = 0; i < take_count; i++) {
        slots[i] = cache->slots[--cache->count];
    }
    return take_count;
}

static int _copool_cache_put(
    copool_cache_t* cache,
    void** slots,
    int count) {
    int put_count = cache->cap - cache->count;
    if (put_count > count) {
        put_count = count;
    }
    for (int i = 0; i < put_count; i++) {
        cache->slots[cache->count++] = slots[i];
    }
    return put_count;
}

static int _copool_shared_take(copool_t* pool, void** slots, int count) {
    spin_lock(&pool->shared_lock);
    int take_count = _copool_cache_take(pool->shared_pool, slots, count);
    spin_unlock(&pool->shared_lock);
    return take_count;
}

static int _copool_shared_put(copool_t* pool, void** slots, int count) {
    spin_lock(&pool->shared_lock);
    int put_count = _copool_cache_put(pool->shared_pool, slots, count);
    spin_unlock(&pool->shared_lock);
    return put_count;
}

static int _copool_init_slots(copool_t* pool, void** slots, int count) {
    int success_count = 0;
    for (int i = 0; i < count; i++) {
        if (pool->ops.init(slots[i], pool->slot_size, pool->ops.ud) == 0) {
            slots[success_count++] = slots[i];
            continue;
        }
        arena_free(pool->arena, &slots[i], 1);
    }
    return success_count;
}

copool_t* copool_create(
    size_t                   slot_size,
    int32_t                  local_pool_count,
    const copool_slot_ops_t* ops) {
    if (slot_size == 0 || local_pool_count < 0 || !ops || !ops->init) {
        return NULL;
    }
    if (local_pool_count > INT32_MAX / COPOOL_LOCAL_CAP) {
        return NULL;
    }
    int32_t shared_cap = local_pool_count * COPOOL_LOCAL_CAP;

    arena_t* arena = arena_create(slot_size);
    if (!arena) {
        return NULL;
    }

    copool_t* pool = (copool_t*)calloc(1, sizeof(copool_t));
    if (!pool) {
        arena_destroy(arena);
        return NULL;
    }

    if (local_pool_count > 0) {
        pool->local_pools = (copool_cache_t**)calloc(
            (size_t)local_pool_count,
            sizeof(copool_cache_t*));
        if (!pool->local_pools) {
            free(pool);
            arena_destroy(arena);
            return NULL;
        }
    }

    pool->shared_pool = _copool_cache_create(shared_cap);
    if (!pool->shared_pool) {
        free(pool->local_pools);
        free(pool);
        arena_destroy(arena);
        return NULL;
    }
    for (int32_t i = 0; i < local_pool_count; i++) {
        pool->local_pools[i] = _copool_cache_create(COPOOL_LOCAL_CAP);
        if (!pool->local_pools[i]) {
            for (int32_t j = 0; j < i; j++) {
                _copool_cache_destroy(pool->local_pools[j]);
            }
            _copool_cache_destroy(pool->shared_pool);
            free(pool->local_pools);
            free(pool);
            arena_destroy(arena);
            return NULL;
        }
    }

    spin_init(&pool->shared_lock);
    pool->local_pool_count = local_pool_count;
    pool->slot_size        = arena_slot_size(arena);
    pool->arena            = arena;
    pool->ops              = *ops;
    return pool;
}

void copool_destroy(copool_t* pool) {
    if (!pool) {
        return;
    }

    for (int32_t i = 0; i < pool->local_pool_count; i++) {
        _copool_cache_destroy(pool->local_pools[i]);
    }
    _copool_cache_destroy(pool->shared_pool);
    free(pool->local_pools);
    arena_destroy(pool->arena);
    free(pool);
}

void* copool_acquire(copool_t* pool, int32_t local_index) {
    if (!pool || local_index < -1 ||
        local_index >= pool->local_pool_count) {
        return NULL;
    }

    if (local_index >= 0) {
        copool_cache_t* local_pool = pool->local_pools[local_index];

        void* slot;
        if (_copool_cache_take(local_pool, &slot, 1) == 1) {
            return slot;
        }

        local_pool->count = _copool_shared_take(
            pool,
            local_pool->slots,
            COPOOL_LOCAL_CAP / 2);
        if (local_pool->count == 0) {
            local_pool->count = arena_alloc(
                pool->arena,
                local_pool->slots,
                COPOOL_LOCAL_CAP / 2);
            local_pool->count = _copool_init_slots(
                pool,
                local_pool->slots,
                local_pool->count);
        }
        if (local_pool->count == 0) {
            return NULL;
        }
        return local_pool->slots[--local_pool->count];
    }

    void* batch[COPOOL_LOCAL_CAP / 2];
    if (_copool_shared_take(pool, batch, 1) == 1) {
        return batch[0];
    }

    int alloc_count =
        arena_alloc(pool->arena, batch, COPOOL_LOCAL_CAP / 2);
    alloc_count = _copool_init_slots(pool, batch, alloc_count);
    if (alloc_count == 0) {
        return NULL;
    }

    int put_count  = _copool_shared_put(pool, &batch[1], alloc_count - 1);
    int free_count = alloc_count - 1 - put_count;
    if (free_count > 0) {
        arena_free(pool->arena, &batch[1 + put_count], free_count);
    }
    return batch[0];
}

void copool_release(
    copool_t* pool,
    int32_t local_index,
    void* ptr) {
    if (!pool || !ptr) {
        return;
    }
    if (local_index < -1 || local_index >= pool->local_pool_count) {
        xylem_loge("<copool> invalid release local=%d", (int)local_index);
        abort();
    }

    if (local_index >= 0) {
        copool_cache_t* local_pool = pool->local_pools[local_index];

        if (_copool_cache_put(local_pool, &ptr, 1) == 1) {
            return;
        }

        void* batch[COPOOL_LOCAL_CAP / 2];
        int take_count =
            _copool_cache_take(local_pool, batch, COPOOL_LOCAL_CAP / 2);

        int put_count = _copool_shared_put(pool, batch, take_count);
        if (put_count < take_count) {
            arena_free(
                pool->arena,
                &batch[put_count],
                take_count - put_count);
        }
        (void)_copool_cache_put(local_pool, &ptr, 1);
        return;
    }

    void* slot = ptr;
    if (_copool_shared_put(pool, &slot, 1) == 0) {
        arena_free(pool->arena, &slot, 1);
    }
}
