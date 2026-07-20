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
#define TEST_LOCAL_POOL_CAP       64

typedef struct {
    copool_t* pool;
    mtx_t     lock;
    void*     active[COPOOL_CONCURRENT_THREADS * COPOOL_CONCURRENT_BATCH];
    size_t    active_count;
} _concurrent_ctx_t;

typedef struct {
    _concurrent_ctx_t* ctx;
    int32_t            local_index;
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
    int32_t          local_pool_count,
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

    return copool_create(slot_size, local_pool_count, &ops);
}

static int _concurrent_thread(void* arg) {
    _concurrent_worker_t* worker = (_concurrent_worker_t*)arg;
    void*                 slots[COPOOL_CONCURRENT_BATCH] = {0};

    for (int round = 0; round < COPOOL_CONCURRENT_ROUNDS; round++) {
        for (int i = 0; i < COPOOL_CONCURRENT_BATCH; i++) {
            slots[i] = copool_acquire(
                worker->ctx->pool,
                worker->local_index);
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
                (uint8_t)(worker->local_index ^ round ^ i);
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
            copool_release(
                worker->ctx->pool,
                worker->local_index,
                slots[i]);
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

static void _drain_local(
    copool_t* pool,
    int32_t local_index,
    int count) {
    for (int i = 0; i < count; i++) {
        void* slot = copool_acquire(pool, local_index);
        ASSERT(slot != NULL);
        copool_release(pool, -1, slot);
    }
}

static void test_local_cache_reuse(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 1, &ctx);
    ASSERT(pool != NULL);
    void* slots[64]    = {0};
    void* recycled[64] = {0};

    for (int i = 0; i < 64; i++) {
        slots[i] = copool_acquire(pool, 0);
        ASSERT(slots[i] != NULL);
    }

    for (int i = 0; i < 64; i++) {
        copool_release(pool, 0, slots[i]);
    }

    for (int i = 0; i < 64; i++) {
        recycled[i] = copool_acquire(pool, 0);
        ASSERT(recycled[i] != NULL);
        ASSERT(_contains_slot(slots, 64, recycled[i]));
        ASSERT(!_contains_slot(recycled, i, recycled[i]));
    }

    for (int i = 0; i < 64; i++) {
        copool_release(pool, -1, recycled[i]);
    }
    copool_destroy(pool);
}

static void test_external_path_refills_shared(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 1);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 1, &ctx);
    ASSERT(pool != NULL);
    void* slots[32] = {0};

    for (int i = 0; i < 32; i++) {
        slots[i] = copool_acquire(pool, -1);
        ASSERT(slots[i] != NULL);
    }
    for (int i = 0; i < 32; i++) {
        copool_release(pool, -1, slots[i]);
    }

    for (int i = 0; i < 32; i++) {
        slots[i] = copool_acquire(pool, -1);
        ASSERT(slots[i] != NULL);
    }
    for (int i = 0; i < 32; i++) {
        copool_release(pool, -1, slots[i]);
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
    void* slots[TEST_LOCAL_POOL_CAP * 2] = {0};

    for (int i = 0; i < TEST_LOCAL_POOL_CAP * 2; i++) {
        slots[i] = copool_acquire(pool, -1);
        ASSERT(slots[i] != NULL);
    }

    for (int i = 0; i < TEST_LOCAL_POOL_CAP - 1; i++) {
        copool_release(pool, -1, slots[i]);
    }

    for (int i = 0; i < TEST_LOCAL_POOL_CAP + 1; i++) {
        copool_release(
            pool,
            0,
            slots[TEST_LOCAL_POOL_CAP - 1 + i]);
    }

    _drain_local(pool, 0, TEST_LOCAL_POOL_CAP / 2 + 1);
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
    ASSERT(_create_pool(page_size, INT32_MAX, &ctx) == NULL);
    ASSERT(copool_create(page_size, 1, NULL) == NULL);
    ASSERT(copool_create(page_size, 1, &missing_init) == NULL);
}

static void test_acquire_invalid_pool_or_index(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 1, &ctx);
    ASSERT(pool != NULL);

    ASSERT(copool_acquire(NULL, 0) == NULL);
    ASSERT(copool_acquire(pool, -2) == NULL);
    ASSERT(copool_acquire(pool, 1) == NULL);

    copool_destroy(pool);
}

static void test_release_null_args(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 0, &ctx);
    ASSERT(pool != NULL);

    copool_release(NULL, -1, NULL);
    copool_release(pool, -1, NULL);
    copool_destroy(NULL);
    copool_destroy(pool);
}

static void test_concurrent_local_caches(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t slot_ctx;
    _slot_ops_ctx_init(&slot_ctx);
    _concurrent_ctx_t ctx = {
        .pool = _create_pool(
            page_size,
            COPOOL_CONCURRENT_THREADS,
            &slot_ctx),
    };
    ASSERT(ctx.pool != NULL);
    ASSERT(mtx_init(&ctx.lock, mtx_plain) == thrd_success);

    thrd_t               threads[COPOOL_CONCURRENT_THREADS];
    _concurrent_worker_t workers[COPOOL_CONCURRENT_THREADS] = {0};
    for (int i = 0; i < COPOOL_CONCURRENT_THREADS; i++) {
        workers[i].ctx         = &ctx;
        workers[i].local_index = i;
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
        _drain_local(
            ctx.pool,
            workers[i].local_index,
            TEST_LOCAL_POOL_CAP / 2);
    }
    mtx_destroy(&ctx.lock);
    copool_destroy(ctx.pool);
}

static void test_slot_init_on_arena_refill(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 1, &ctx);
    ASSERT(pool != NULL);

    void* slot = copool_acquire(pool, 0);
    ASSERT(slot != NULL);
    ASSERT(
        atomic_load_explicit(&ctx.init_count, memory_order_relaxed) ==
        TEST_LOCAL_POOL_CAP / 2);

    copool_release(pool, 0, slot);
    copool_destroy(pool);
}

static void test_slot_return_stays_hot(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(page_size, 1, &ctx);
    ASSERT(pool != NULL);

    void* slot = copool_acquire(pool, 0);
    ASSERT(slot != NULL);
    ((uint8_t*)slot)[0] = 0x5a;
    int init_count =
        atomic_load_explicit(&ctx.init_count, memory_order_relaxed);
    copool_release(pool, 0, slot);
    void* recycled = copool_acquire(pool, 0);
    ASSERT(recycled == slot);
    ASSERT(((uint8_t*)recycled)[0] == 0x5a);
    ASSERT(
        atomic_load_explicit(&ctx.init_count, memory_order_relaxed) ==
        init_count);

    copool_release(pool, 0, recycled);
    copool_destroy(pool);
}

static void test_slot_init_failure_isolated(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    atomic_store_explicit(&ctx.fail_init, 1, memory_order_relaxed);
    copool_t* pool = _create_pool(page_size, 1, &ctx);
    ASSERT(pool != NULL);

    void* slot = copool_acquire(pool, 0);
    ASSERT(slot != NULL);
    void* failed = atomic_load_explicit(
        &ctx.failed_init_ptr,
        memory_order_relaxed);
    ASSERT(failed != NULL);
    ASSERT(slot != failed);
    ASSERT(
        atomic_load_explicit(&ctx.init_count, memory_order_relaxed) ==
        TEST_LOCAL_POOL_CAP / 2);

    int   cached_count = TEST_LOCAL_POOL_CAP / 2 - 2;
    void* cached[TEST_LOCAL_POOL_CAP / 2] = {0};
    for (int i = 0; i < cached_count; i++) {
        cached[i] = copool_acquire(pool, 0);
        ASSERT(cached[i] != NULL);
        ASSERT(cached[i] != failed);
    }

    copool_release(pool, -1, slot);
    for (int i = 0; i < cached_count; i++) {
        copool_release(pool, -1, cached[i]);
    }
    copool_destroy(pool);
}

static void test_aligned_callback_size(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    size_t slot_size = page_size + 1;
    _slot_ops_ctx_t ctx;
    _slot_ops_ctx_init(&ctx);
    copool_t* pool = _create_pool(slot_size, 1, &ctx);
    ASSERT(pool != NULL);

    void* slot = copool_acquire(pool, 0);
    ASSERT(slot != NULL);
    ASSERT(
        atomic_load_explicit(&ctx.init_size, memory_order_relaxed) ==
        page_size * 2);
    copool_release(pool, 0, slot);
    copool_destroy(pool);
}

int main(void) {
    test_create_invalid_args();
    test_local_cache_reuse();
    test_external_path_refills_shared();
    test_local_overflow_reaches_shared_and_arena();
    test_acquire_invalid_pool_or_index();
    test_release_null_args();
    test_concurrent_local_caches();
    test_slot_init_on_arena_refill();
    test_slot_return_stays_hot();
    test_slot_init_failure_isolated();
    test_aligned_callback_size();
    return 0;
}
