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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    copool_t* pool = copool_create(page_size, 64);
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

    copool_t* pool = copool_create(page_size, 64);
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

    copool_t* pool = copool_create(page_size, 1);
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

    ASSERT(copool_create(0, 1) == NULL);
    ASSERT(copool_create(page_size, -1) == NULL);
}

static void test_alloc_invalid_args(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    copool_t* pool = copool_create(page_size, 1);
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

    copool_t* pool = copool_create(page_size, 1);
    ASSERT(pool != NULL);

    copool_free(NULL, NULL, NULL, page_size);
    copool_free(pool, NULL, NULL, page_size);
    copool_destroy(NULL);
    copool_destroy(pool);
}

static void test_concurrent_local_caches(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    _concurrent_ctx_t ctx = {
        .pool = copool_create(page_size, COPOOL_CACHE_CAP),
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

static int _free_zero_size_child(void) {
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    copool_t* pool = copool_create(page_size, 1);
    ASSERT(pool != NULL);
    void* slot = copool_alloc(pool, NULL, page_size);
    ASSERT(slot != NULL);

    copool_free(pool, NULL, slot, 0);
    copool_destroy(pool);
    return 0;
}

static void test_free_zero_size_aborts(const char* executable) {
    char command[4096];
    int  command_len = snprintf(
        command,
        sizeof(command),
        "\"%s\" --free-zero-size",
        executable);
    ASSERT(command_len > 0);
    ASSERT((size_t)command_len < sizeof(command));

    int rc = system(command);
    ASSERT(rc != -1);
    ASSERT(rc != 0);
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--free-zero-size") == 0) {
        return _free_zero_size_child();
    }

    test_local_cache_reuse();
    test_external_path_refills_shared();
    test_local_overflow_reaches_shared_and_arena();
    test_create_invalid_args();
    test_alloc_invalid_args();
    test_free_null_args();
    test_concurrent_local_caches();
    test_free_zero_size_aborts(argv[0]);
    return 0;
}
