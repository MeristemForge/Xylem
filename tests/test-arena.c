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
#include "runtime/arena.h"

#include <stdint.h>

#define MIB                      (1024U * 1024U)
#define ARENA_CONCURRENT_THREADS 4
#define ARENA_CONCURRENT_BATCH   8
#define ARENA_CONCURRENT_ROUNDS  200

typedef struct {
    arena_t* arena;
    mtx_t    lock;
    void*    active[ARENA_CONCURRENT_THREADS * ARENA_CONCURRENT_BATCH];
    size_t   active_count;
} _concurrent_ctx_t;

typedef struct {
    _concurrent_ctx_t* ctx;
    uint8_t            id;
} _concurrent_worker_t;

static int _concurrent_thread(void* arg) {
    _concurrent_worker_t* worker = (_concurrent_worker_t*)arg;
    _concurrent_ctx_t*    ctx    = worker->ctx;
    void*                 slots[ARENA_CONCURRENT_BATCH] = {0};

    for (int round = 0; round < ARENA_CONCURRENT_ROUNDS; round++) {
        ASSERT(
            arena_alloc(ctx->arena, slots, ARENA_CONCURRENT_BATCH) ==
            ARENA_CONCURRENT_BATCH);

        ASSERT(mtx_lock(&ctx->lock) == thrd_success);
        for (int i = 0; i < ARENA_CONCURRENT_BATCH; i++) {
            uint8_t* bytes = (uint8_t*)slots[i];
            bytes[0] = (uint8_t)(worker->id ^ (uint8_t)round ^ (uint8_t)i);

            for (size_t j = 0; j < ctx->active_count; j++) {
                ASSERT(ctx->active[j] != slots[i]);
            }
            ASSERT(
                ctx->active_count <
                ARENA_CONCURRENT_THREADS * ARENA_CONCURRENT_BATCH);
            ctx->active[ctx->active_count++] = slots[i];
        }
        ASSERT(mtx_unlock(&ctx->lock) == thrd_success);

        thrd_yield();

        ASSERT(mtx_lock(&ctx->lock) == thrd_success);
        for (int i = 0; i < ARENA_CONCURRENT_BATCH; i++) {
            size_t found = ctx->active_count;
            for (size_t j = 0; j < ctx->active_count; j++) {
                if (ctx->active[j] == slots[i]) {
                    found = j;
                    break;
                }
            }
            ASSERT(found < ctx->active_count);
            ctx->active[found] = ctx->active[--ctx->active_count];
        }
        ASSERT(mtx_unlock(&ctx->lock) == thrd_success);

        arena_free(ctx->arena, slots, ARENA_CONCURRENT_BATCH);
        thrd_yield();
    }
    return 0;
}

static void test_create_limits(void) {
    ASSERT(arena_create(0) == NULL);
    ASSERT(arena_create(MIB + 1U) == NULL);

    arena_t* arena = arena_create(MIB);
    ASSERT(arena != NULL);
    arena_destroy(arena);
}

static void test_alloc_free_realloc(void) {
    arena_t* arena = arena_create(123);
    ASSERT(arena != NULL);

    void*  slots[32] = {0};
    size_t page_size = platform_vmem_page_size();
    ASSERT(arena_alloc(arena, slots, 32) == 32);
    for (int i = 0; i < 32; i++) {
        ASSERT(slots[i] != NULL);
        ASSERT((uintptr_t)slots[i] % page_size == 0);
        for (int j = 0; j < i; j++) {
            ASSERT(slots[i] != slots[j]);
        }
        uint8_t* bytes = (uint8_t*)slots[i];
        bytes[0]       = (uint8_t)i;
        bytes[122]     = (uint8_t)(i + 1);
        ASSERT(bytes[0] == (uint8_t)i);
        ASSERT(bytes[122] == (uint8_t)(i + 1));
    }

    arena_free(arena, slots, 32);

    void* recycled[32] = {0};
    ASSERT(arena_alloc(arena, recycled, 32) == 32);
    for (int i = 0; i < 32; i++) {
        int match_count = 0;
        for (int j = 0; j < 32; j++) {
            if (slots[i] == recycled[j]) {
                match_count++;
            }
        }
        ASSERT(match_count == 1);
    }
    for (int i = 0; i < 32; i++) {
        ASSERT(recycled[i] != NULL);
        uint8_t* bytes = (uint8_t*)recycled[i];
        bytes[0]       = (uint8_t)(i + 2);
        bytes[122]     = (uint8_t)(i + 3);
        ASSERT(bytes[0] == (uint8_t)(i + 2));
        ASSERT(bytes[122] == (uint8_t)(i + 3));
    }

    arena_free(arena, recycled, 32);
    arena_destroy(arena);
}

static void test_null_args(void) {
    void* slots[1] = {NULL};

    ASSERT(arena_alloc(NULL, slots, 1) == 0);

    arena_t* arena = arena_create(1);
    ASSERT(arena != NULL);
    ASSERT(arena_alloc(arena, NULL, 1) == 0);
    ASSERT(arena_alloc(arena, slots, 0) == 0);
    ASSERT(arena_alloc(arena, slots, -1) == 0);

    arena_free(NULL, slots, 1);
    arena_free(arena, NULL, 1);
    arena_free(arena, slots, 0);
    arena_free(arena, slots, -1);
    arena_destroy(NULL);
    arena_destroy(arena);
}

static void test_growth(void) {
    arena_t* arena = arena_create(MIB);
    ASSERT(arena != NULL);

    void* slots[65] = {0};
    ASSERT(arena_alloc(arena, slots, 65) == 65);
    for (int i = 0; i < 65; i++) {
        for (int j = 0; j < i; j++) {
            ASSERT(slots[i] != slots[j]);
        }
        uint8_t* bytes  = (uint8_t*)slots[i];
        bytes[0]        = (uint8_t)i;
        bytes[MIB - 1U] = (uint8_t)(i + 1);
        ASSERT(bytes[0] == (uint8_t)i);
        ASSERT(bytes[MIB - 1U] == (uint8_t)(i + 1));
    }

    arena_free(arena, slots, 65);
    arena_destroy(arena);
}

static void test_destroy_with_allocated_slot(void) {
    arena_t* arena = arena_create(4096);
    ASSERT(arena != NULL);

    void* slot = NULL;
    ASSERT(arena_alloc(arena, &slot, 1) == 1);
    ASSERT(slot != NULL);
    ((uint8_t*)slot)[0] = 0x5a;
    arena_destroy(arena);
}

static void test_concurrent_alloc_free(void) {
    _concurrent_ctx_t ctx = {
        .arena = arena_create(64),
    };
    ASSERT(ctx.arena != NULL);
    ASSERT(mtx_init(&ctx.lock, mtx_plain) == thrd_success);

    thrd_t               threads[ARENA_CONCURRENT_THREADS];
    _concurrent_worker_t workers[ARENA_CONCURRENT_THREADS];
    for (int i = 0; i < ARENA_CONCURRENT_THREADS; i++) {
        workers[i].ctx = &ctx;
        workers[i].id  = (uint8_t)i;
        ASSERT(
            thrd_create(&threads[i], _concurrent_thread, &workers[i]) ==
            thrd_success);
    }

    for (int i = 0; i < ARENA_CONCURRENT_THREADS; i++) {
        int result = -1;
        ASSERT(thrd_join(threads[i], &result) == thrd_success);
        ASSERT(result == 0);
    }
    ASSERT(ctx.active_count == 0);

    mtx_destroy(&ctx.lock);
    arena_destroy(ctx.arena);
}

int main(void) {
    test_create_limits();
    test_alloc_free_realloc();
    test_null_args();
    test_growth();
    test_destroy_with_allocated_slot();
    test_concurrent_alloc_free();
    return 0;
}
