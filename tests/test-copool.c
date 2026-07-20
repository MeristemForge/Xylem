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

#include "xylem/xylem-threads.h"

#include "assert.h"
#include "platform/platform-vmem.h"
#include "runtime/copool.h"

#include <stdatomic.h>
#include <stdint.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define COPOOL_CONCURRENT_THREADS 4
#define COPOOL_CONCURRENT_BATCH   16
#define COPOOL_CONCURRENT_ROUNDS  500

typedef struct {
    copool_t* pool;
    mtx_t     lock;
    void*     active[COPOOL_CONCURRENT_THREADS * COPOOL_CONCURRENT_BATCH];
    size_t    active_count;
} _concurrent_ctx_t;

typedef struct {
    _concurrent_ctx_t* ctx;
    copool_cache_t     cache;
    size_t             slot_size;
    uint8_t            id;
} _concurrent_worker_t;

typedef struct {
    _Atomic int    init_count;
    _Atomic int    fail_init;
    _Atomic size_t init_size;
    _Atomic(void*) failed_init_ptr;
    size_t         expected_size;
} _slot_ops_ctx_t;

#if defined(_WIN32)
static void _assert_slot_state(
    const void* ptr,
    DWORD       expected_state,
    DWORD       expected_protection) {
    MEMORY_BASIC_INFORMATION info;

    ASSERT(VirtualQuery(ptr, &info, sizeof(info)) == sizeof(info));
    ASSERT(info.State == expected_state);
    ASSERT((info.Protect & 0xffU) == expected_protection);
}
#endif

static void _slot_ops_ctx_init(_slot_ops_ctx_t* ctx) {
    atomic_init(&ctx->init_count, 0);
    atomic_init(&ctx->fail_init, 0);
    atomic_init(&ctx->init_size, 0);
    atomic_init(&ctx->failed_init_ptr, NULL);
    ctx->expected_size = 0;
}

static int _slot_consume_failure(_Atomic int* budget) {
    int remaining = atomic_load_explicit(budget, memory_order_relaxed);
    while (remaining > 0) {
        if (atomic_compare_exchange_weak_explicit(
                budget,
                &remaining,
                remaining - 1,
                memory_order_relaxed,
                memory_order_relaxed)) {
            return 1;
        }
    }
    return 0;
}

static int _slot_init(void* ptr, size_t size, void* ud) {
    _slot_ops_ctx_t* ctx = (_slot_ops_ctx_t*)ud;

    ASSERT(size == ctx->expected_size);
#if defined(_WIN32)
    _assert_slot_state(ptr, MEM_RESERVE, 0);
#endif

    atomic_fetch_add_explicit(&ctx->init_count, 1, memory_order_relaxed);
    atomic_store_explicit(&ctx->init_size, size, memory_order_relaxed);
    if (_slot_consume_failure(&ctx->fail_init)) {
        atomic_store_explicit(
            &ctx->failed_init_ptr,
            ptr,
            memory_order_relaxed);
        return -1;
    }
    int rc = platform_vmem_commit(ptr, size);
#if defined(_WIN32)
    if (rc == 0) {
        _assert_slot_state(ptr, MEM_COMMIT, PAGE_READWRITE);
    }
#endif
    return rc;
}

static copool_t* _create_pool(
    size_t           slot_size,
    int32_t          shared_cap,
    _slot_ops_ctx_t* ctx) {
    copool_slot_ops_t ops = {
        .init = _slot_init,
        .ud   = ctx,
    };

    size_t page_size = platform_vmem_page_size();
    if (slot_size > 0) {
        ctx->expected_size =
            ((slot_size + page_size - 1) / page_size) * page_size;
    }

    return copool_create(slot_size, shared_cap, &ops);
}

static int _concurrent_thread(void* arg) {
    _concurrent_worker_t* worker = (_concurrent_worker_t*)arg;
    void*                 slots[COPOOL_CONCURRENT_BATCH] = {0};

    for (int round = 0; round < COPOOL_CONCURRENT_ROUNDS; round++) {
        for (int i = 0; i < COPOOL_CONCURRENT_BATCH; i++) {
            slots[i] = copool_alloc(
                worker->ctx->pool,
                &worker->cache,
                worker->slot_size);
            ASSERT(slots[i] != NULL);
        }

        ASSERT(mtx_lock(&worker->ctx->lock) == thrd_success);
        for (int i = 0; i < COPOOL_CONCURRENT_BATCH; i++) {
            for (size_t j = 0; j < worker->ctx->active_count; j++) {
                ASSERT(worker->ctx->active[j] != slots[i]);
            }
            ASSERT(
                worker->ctx->active_count <
                COPOOL_CONCURRENT_THREADS * COPOOL_CONCURRENT_BATCH);
            worker->ctx->active[worker->ctx->active_count++] = slots[i];
            ((uint8_t*)slots[i])[0] =
                (uint8_t)(worker->id ^ (uint8_t)round ^ (uint8_t)i);
        }
        ASSERT(mtx_unlock(&worker->ctx->lock) == thrd_success);

        thrd_yield();

        ASSERT(mtx_lock(&worker->ctx->lock) == thrd_success);
        for (int i = 0; i < COPOOL_CONCURRENT_BATCH; i++) {
            size_t found = worker->ctx->active_count;
            for (size_t j = 0; j < worker->ctx->active_count; j++) {
                if (worker->ctx->active[j] == slots[i]) {
                    found = j;
                    break;
                }
            }
            ASSERT(found < worker->ctx->active_count);
            worker->ctx->active[found] =
                worker->ctx->active[--worker->ctx->active_count];
        }
        ASSERT(mtx_unlock(&worker->ctx->lock) == thrd_success);

        for (int i = 0; i < COPOOL_CONCURRENT_BATCH; i++) {
            copool_free(
                worker->ctx->pool,
                &worker->cache,
                slots[i],
                worker->slot_size);
        }
    }
    return 0;
}

static int _contains_slot(void** slots, int count, void* slot) {
    for (int i = 0; i < count; i++) {
        if (slots[i] == slot) {
            return 1;
        }
    }
    return 0;
}

static void _drain_cache(copool_t* pool, copool_cache_t* cache, size_t size) {
    while (cache->count > 0) {
        void* slot = copool_alloc(pool, cache, size);
        ASSERT(slot != NULL);
        copool_free(pool, NULL, slot, size);
    }
}

static void test_local_cache_reuse(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 64, &ctx);
    ASSERT(pool != NULL);
    copool_cache_t cache        = {0};
    void*          slots[64]    = {0};
    void*          recycled[64] = {0};

    for (int i = 0; i < 64; i++) {
        slots[i] = copool_alloc(pool, &cache, page_size);
        ASSERT(slots[i] != NULL);
    }
    ASSERT(cache.count == 0);

    for (int i = 0; i < 64; i++) {
        copool_free(pool, &cache, slots[i], page_size);
    }
    ASSERT(cache.count == COPOOL_CACHE_CAP);

    for (int i = 0; i < 64; i++) {
        recycled[i] = copool_alloc(pool, &cache, page_size);
        ASSERT(recycled[i] != NULL);
        ASSERT(_contains_slot(slots, 64, recycled[i]));
        ASSERT(!_contains_slot(recycled, i, recycled[i]));
    }
    ASSERT(cache.count == 0);

    for (int i = 0; i < 64; i++) {
        copool_free(pool, NULL, recycled[i], page_size);
    }
    copool_destroy(pool);
}

static void test_external_path_refills_shared(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 1);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 64, &ctx);
    ASSERT(pool != NULL);
    void* slots[32] = {0};

    for (int i = 0; i < 32; i++) {
        slots[i] = copool_alloc(pool, NULL, page_size - 1);
        ASSERT(slots[i] != NULL);
    }
    for (int i = 0; i < 32; i++) {
        copool_free(pool, NULL, slots[i], page_size - 1);
    }

    for (int i = 0; i < 32; i++) {
        slots[i] = copool_alloc(pool, NULL, page_size - 1);
        ASSERT(slots[i] != NULL);
    }
    for (int i = 0; i < 32; i++) {
        copool_free(pool, NULL, slots[i], page_size - 1);
    }
    copool_destroy(pool);
}

static void test_local_overflow_reaches_shared_and_arena(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 1, &ctx);
    ASSERT(pool != NULL);
    void* slots[65] = {0};

    for (int i = 0; i < 65; i++) {
        slots[i] = copool_alloc(pool, NULL, page_size);
        ASSERT(slots[i] != NULL);
    }

    void* shared_slot = copool_alloc(pool, NULL, page_size);
    ASSERT(shared_slot != NULL);

    copool_cache_t cache = {0};
    for (int i = 0; i < 65; i++) {
        copool_free(pool, &cache, slots[i], page_size);
    }
    ASSERT(cache.count == 33);

    copool_free(pool, NULL, shared_slot, page_size);
    _drain_cache(pool, &cache, page_size);
    ASSERT(cache.count == 0);
    copool_destroy(pool);
}

static void test_create_invalid_args(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_slot_ops_t missing_init = {
        .ud = &ctx,
    };

    ASSERT(_create_pool(0, 1, &ctx) == NULL);
    ASSERT(_create_pool(page_size, -1, &ctx) == NULL);
    ASSERT(copool_create(page_size, 1, NULL) == NULL);
    ASSERT(copool_create(page_size, 1, &missing_init) == NULL);
}

static void test_alloc_invalid_args(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 1, &ctx);
    ASSERT(pool != NULL);
    copool_cache_t cache = {0};

    ASSERT(copool_alloc(NULL, &cache, page_size) == NULL);
    ASSERT(copool_alloc(pool, &cache, 0) == NULL);
    ASSERT(copool_alloc(pool, &cache, page_size + 1) == NULL);

    copool_destroy(pool);
}

static void test_free_null_args(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 1, &ctx);
    ASSERT(pool != NULL);

    copool_free(NULL, NULL, NULL, page_size);
    copool_free(pool, NULL, NULL, page_size);
    copool_destroy(NULL);
    copool_destroy(pool);
}

static void test_concurrent_local_caches(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t slot_ctx;
    _slot_ops_ctx_init(&slot_ctx);
    _concurrent_ctx_t ctx = {
        .pool = _create_pool(page_size, COPOOL_CACHE_CAP, &slot_ctx),
    };
    ASSERT(ctx.pool != NULL);
    ASSERT(mtx_init(&ctx.lock, mtx_plain) == thrd_success);

    thrd_t               threads[COPOOL_CONCURRENT_THREADS];
    _concurrent_worker_t workers[COPOOL_CONCURRENT_THREADS] = {0};
    for (int i = 0; i < COPOOL_CONCURRENT_THREADS; i++) {
        workers[i].ctx       = &ctx;
        workers[i].slot_size = page_size;
        workers[i].id        = (uint8_t)i;
        ASSERT(
            thrd_create(&threads[i], _concurrent_thread, &workers[i]) ==
            thrd_success);
    }

    for (int i = 0; i < COPOOL_CONCURRENT_THREADS; i++) {
        int result = -1;
        ASSERT(thrd_join(threads[i], &result) == thrd_success);
        ASSERT(result == 0);
    }
    ASSERT(ctx.active_count == 0);
    for (int i = 0; i < COPOOL_CONCURRENT_THREADS; i++) {
        _drain_cache(ctx.pool, &workers[i].cache, page_size);
        ASSERT(workers[i].cache.count == 0);
    }
    mtx_destroy(&ctx.lock);
    copool_destroy(ctx.pool);
}

static void test_slot_init_on_arena_refill(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t*      pool  = _create_pool(page_size, 64, &ctx);
    copool_cache_t cache = {0};
    ASSERT(pool != NULL);

    void* slot = copool_alloc(pool, &cache, page_size);
    ASSERT(slot != NULL);
    ASSERT(
        atomic_load_explicit(&ctx.init_count, memory_order_relaxed) ==
        COPOOL_CACHE_CAP / 2);

    copool_free(pool, &cache, slot, page_size);
    copool_destroy(pool);
}

static void test_slot_return_stays_hot(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t*      pool  = _create_pool(page_size, 64, &ctx);
    copool_cache_t cache = {0};
    ASSERT(pool != NULL);

    void* slot = copool_alloc(pool, &cache, page_size);
    ASSERT(slot != NULL);
    ((uint8_t*)slot)[0] = 0x5a;
    int init_count =
        atomic_load_explicit(&ctx.init_count, memory_order_relaxed);
    copool_free(pool, &cache, slot, page_size);
    void* recycled = copool_alloc(pool, &cache, page_size);
    ASSERT(recycled == slot);
    ASSERT(((uint8_t*)recycled)[0] == 0x5a);
    ASSERT(
        atomic_load_explicit(&ctx.init_count, memory_order_relaxed) ==
        init_count);

    copool_free(pool, &cache, recycled, page_size);
    copool_destroy(pool);
}

static void test_slot_init_failure_isolated(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    atomic_store_explicit(&ctx.fail_init, 1, memory_order_relaxed);
    copool_t*      pool  = _create_pool(page_size, 64, &ctx);
    copool_cache_t cache = {0};
    ASSERT(pool != NULL);

    void* slot = copool_alloc(pool, &cache, page_size);
    ASSERT(slot != NULL);
    void* failed = atomic_load_explicit(
        &ctx.failed_init_ptr,
        memory_order_relaxed);
    ASSERT(failed != NULL);
    ASSERT(slot != failed);
    ASSERT(!_contains_slot(cache.slots, cache.count, failed));
    ASSERT(cache.count == COPOOL_CACHE_CAP / 2 - 2);
    ASSERT(
        atomic_load_explicit(&ctx.init_count, memory_order_relaxed) ==
        COPOOL_CACHE_CAP / 2);

    int   cached_count = cache.count;
    void* cached[COPOOL_CACHE_CAP / 2] = {0};
    for (int i = 0; i < cached_count; i++) {
        cached[i] = copool_alloc(pool, &cache, page_size);
        ASSERT(cached[i] != NULL);
        ASSERT(cached[i] != failed);
    }
    ASSERT(cache.count == 0);

    copool_free(pool, NULL, slot, page_size);
    for (int i = 0; i < cached_count; i++) {
        copool_free(pool, NULL, cached[i], page_size);
    }
    copool_destroy(pool);
}

static void test_logical_max_uses_aligned_callback_size(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    size_t logical_size = page_size + 1;
    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t*      pool  = _create_pool(logical_size, 64, &ctx);
    copool_cache_t cache = {0};
    ASSERT(pool != NULL);

    void* slot = copool_alloc(pool, &cache, logical_size);
    ASSERT(slot != NULL);
    ASSERT(
        atomic_load_explicit(&ctx.init_size, memory_order_relaxed) ==
        page_size * 2);
    ASSERT(copool_alloc(pool, &cache, logical_size + 1) == NULL);

    copool_free(pool, &cache, slot, logical_size);
    copool_destroy(pool);
}

int main(void) {
    test_create_invalid_args();
    test_local_cache_reuse();
    test_external_path_refills_shared();
    test_local_overflow_reaches_shared_and_arena();
    test_alloc_invalid_args();
    test_free_null_args();
    test_concurrent_local_caches();
    test_slot_init_on_arena_refill();
    test_slot_return_stays_hot();
    test_slot_init_failure_isolated();
    test_logical_max_uses_aligned_callback_size();
    return 0;
}
