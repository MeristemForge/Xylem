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

#include "assert.h"
#include "xylem/xylem-threads.h"

#include "platform/platform-info.h"
#include "platform/platform-vmem.h"
#include "runtime/arena.h"
#include "runtime/copool.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CORO_STACK_FRAME_SIZE    8192
#define CORO_STACK_DEPTH         8

#define MIGRATION_FIRST_READY  (1 << 0)
#define MIGRATION_SECOND_READY (1 << 1)
#define MIGRATION_FIRST_GO     (1 << 2)
#define MIGRATION_FIRST_DONE   (1 << 3)
#define MIGRATION_SECOND_GO    (1 << 4)
#define MIGRATION_SECOND_DONE  (1 << 5)

#if defined(_WIN32) && defined(MCO_USE_FIBERS)
#define TEST_MCO_WINDOWS_FIBER 1
#elif defined(_WIN32) && !defined(MCO_USE_FIBERS) &&       \
    !defined(MCO_USE_UCONTEXT) && !defined(MCO_USE_ASM) && \
    !defined(MCO_USE_ASYNCIFY) && !defined(__x86_64__) && !defined(_M_X64)
#define TEST_MCO_WINDOWS_FIBER 1
#endif

#if defined(_WIN32) && !defined(MCO_USE_FIBERS) &&              \
    !defined(MCO_USE_UCONTEXT) && !defined(MCO_USE_ASYNCIFY) && \
    (defined(__x86_64__) || defined(_M_X64))
#define TEST_MCO_WINDOWS_ASM 1
#endif

#if defined(TEST_MCO_WINDOWS_ASM) && defined(_MSC_VER)
#define TEST_MCO_WINDOWS_SEH 1
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_MCO_ASAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define TEST_MCO_ASAN 1
#endif

#if defined(TEST_MCO_WINDOWS_ASM)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#endif

#if defined(TEST_MCO_WINDOWS_ASM)
#include <malloc.h>
#endif

typedef struct {
    arena_t*         arena;
    copool_local_t*  local_pool;
    copool_shared_t* shared_pool;
    atomic_int       alloc_calls;
    atomic_int       dealloc_calls;
} _coro_fixture_t;

typedef struct {
    arena_t* arena;
} _arena_fixture_t;

typedef struct {
    atomic_uintptr_t co;
    mtx_t            lock;
    cnd_t            condition;
    atomic_int       state;
    atomic_int       entry_error;
    atomic_uintptr_t first_tid;
    atomic_uintptr_t second_tid;
    atomic_uint      checksum;
} _migration_ctx_t;

typedef struct {
    uint8_t  seed;
    uint32_t checksum;
    int      deep;
    int      allocate;
    void*    stack_limit;
} _stack_entry_ctx_t;

#if defined(TEST_MCO_WINDOWS_ASM)
typedef struct {
    void* stack_base;
    void* stack_limit;
} _teb_entry_ctx_t;

typedef struct {
    arena_t* arena;
    void*    slots[2];
    int      next_slot;
} _ordered_fixture_t;

typedef struct {
    _ordered_fixture_t* fixture;
    void*               stack_base_before;
    void*               stack_base_after;
    void*               stack_limit_before;
    void*               stack_limit_after;
    mco_result          create_result;
    mco_result          destroy_result;
} _fresh_child_ctx_t;
#endif

#if defined(TEST_MCO_WINDOWS_SEH) && !defined(TEST_MCO_ASAN)
typedef struct _overflow_ctx_s _overflow_ctx_t;
typedef uint32_t (*_overflow_recurse_fn_t)(_overflow_ctx_t* ctx, uint8_t seed);

struct _overflow_ctx_s {
    _overflow_recurse_fn_t recurse;
    uint64_t*              sentinel;
    uint64_t               sentinel_value;
    volatile int           keep_recursing;
    DWORD                  exception_code;
    uint32_t               checksum;
    int                    reset_result;
    int                    returned;
    int                    calls;
};
#endif

static void _empty_entry(mco_coro* co) {
    (void)co;
}

static void* _arena_alloc_cb(
    size_t             size,
    void*              allocator_data,
    mco_storage_state* storage_state) {
    (void)size;
    _arena_fixture_t* fixture = (_arena_fixture_t*)allocator_data;
    void*             slot    = NULL;

    if (arena_alloc(fixture->arena, &slot, 1) != 1) {
        return NULL;
    }
    *storage_state = MCO_STORAGE_FRESH;
    return slot;
}

static void _arena_dealloc_cb(
    void*             ptr,
    size_t            size,
    void*             allocator_data,
    mco_storage_state storage_state) {
    (void)size;
    (void)storage_state;
    _arena_fixture_t* fixture = (_arena_fixture_t*)allocator_data;

    arena_free(fixture->arena, &ptr, 1);
}

static uint32_t _touch_stack(int depth, uint8_t seed) {
    volatile uint8_t frame[CORO_STACK_FRAME_SIZE];
    uint32_t         checksum = 0;

    for (size_t i = 0; i < CORO_STACK_FRAME_SIZE; i += 4096U) {
        frame[i] = (uint8_t)(seed + (uint8_t)(i / 4096U));
        checksum += frame[i];
    }
    frame[CORO_STACK_FRAME_SIZE - 1] = (uint8_t)(seed ^ 0xa5U);
    checksum += frame[CORO_STACK_FRAME_SIZE - 1];
    if (depth > 0) {
        checksum += _touch_stack(depth - 1, (uint8_t)(seed + 1U));
    }
    for (size_t i = 0; i < CORO_STACK_FRAME_SIZE; i += 4096U) {
        checksum += frame[i];
    }
    checksum += frame[CORO_STACK_FRAME_SIZE - 1];
    return checksum;
}

static void _migration_entry(mco_coro* co) {
    _migration_ctx_t* ctx = (_migration_ctx_t*)mco_get_user_data(co);

    atomic_store(&ctx->first_tid, (uintptr_t)platform_info_gettid());
    atomic_fetch_add(&ctx->checksum, _touch_stack(CORO_STACK_DEPTH, 0x21U));
    if (mco_yield(co) != MCO_SUCCESS) {
        atomic_store(&ctx->entry_error, 1);
        return;
    }
    atomic_store(&ctx->second_tid, (uintptr_t)platform_info_gettid());
    atomic_fetch_add(&ctx->checksum, _touch_stack(CORO_STACK_DEPTH, 0x61U));
}

static void _stack_entry(mco_coro* co) {
    _stack_entry_ctx_t* ctx = (_stack_entry_ctx_t*)mco_get_user_data(co);

    if (ctx->deep) {
        ctx->checksum = _touch_stack(CORO_STACK_DEPTH, ctx->seed);
    } else {
        ctx->checksum = (uint32_t)ctx->seed + 1U;
    }
    if (ctx->allocate) {
#if defined(TEST_MCO_WINDOWS_ASM)
        volatile uint8_t* frame =
            (volatile uint8_t*)_alloca(96U * 1024U);
        for (size_t i = 0; i < 96U * 1024U; i += 4096U) {
            frame[i] = (uint8_t)i;
        }
#endif
        void* ptr = calloc(80, 1);
        if (ptr == NULL) {
            abort();
        }
        free(ptr);
    }
#if defined(TEST_MCO_WINDOWS_ASM)
    ctx->stack_limit = ((NT_TIB*)NtCurrentTeb())->StackLimit;
#endif
}

#if defined(TEST_MCO_WINDOWS_ASM)
static void _teb_entry(mco_coro* co) {
    _teb_entry_ctx_t* ctx = (_teb_entry_ctx_t*)mco_get_user_data(co);
    NT_TIB*           tib = (NT_TIB*)NtCurrentTeb();

    ctx->stack_base  = tib->StackBase;
    ctx->stack_limit = tib->StackLimit;
}
#endif

#if defined(TEST_MCO_WINDOWS_SEH) && !defined(TEST_MCO_ASAN)
static __declspec(noinline) uint32_t
_overflow_recurse(_overflow_ctx_t* ctx, uint8_t seed) {
    volatile uint8_t frame[CORO_STACK_FRAME_SIZE];
    uint32_t         checksum = 0;

    ctx->calls++;
    for (size_t i = 0; i < CORO_STACK_FRAME_SIZE; i += 4096U) {
        frame[i] = (uint8_t)(seed + (uint8_t)(i / 4096U));
        checksum += frame[i];
    }
    frame[CORO_STACK_FRAME_SIZE - 1] = (uint8_t)(seed ^ 0x5aU);
    checksum += frame[CORO_STACK_FRAME_SIZE - 1];
    if (ctx->keep_recursing != 0) {
        checksum += ctx->recurse(ctx, (uint8_t)(seed + 1U));
    }
    checksum += frame[0];
    checksum += frame[4096];
    checksum += frame[CORO_STACK_FRAME_SIZE - 1];
    return checksum;
}

static int _overflow_filter(DWORD code, _overflow_ctx_t* ctx) {
    ctx->exception_code = code;
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER
                                            : EXCEPTION_CONTINUE_SEARCH;
}

static void _overflow_entry(mco_coro* co) {
    _overflow_ctx_t* ctx = (_overflow_ctx_t*)mco_get_user_data(co);

    __try {
        ctx->checksum = ctx->recurse(ctx, 0x31U);
    } __except (_overflow_filter(GetExceptionCode(), ctx)) {
        ctx->reset_result = _resetstkoflw();
    }
    ctx->returned = 1;
}
#endif

static void* _coro_alloc_cb(
    size_t             size,
    void*              allocator_data,
    mco_storage_state* storage_state) {
    (void)size;
    _coro_fixture_t* fixture = (_coro_fixture_t*)allocator_data;

    atomic_fetch_add(&fixture->alloc_calls, 1);
    copool_slot_t slot;
    if (fixture->local_pool &&
        copool_local_take(fixture->local_pool, &slot, 1) == 1) {
        *storage_state =
            slot.state == COPOOL_SLOT_FRESH ? MCO_STORAGE_FRESH
                                            : MCO_STORAGE_REUSABLE;
        return slot.ptr;
    }
    if (fixture->shared_pool &&
        copool_shared_take(fixture->shared_pool, &slot, 1) == 1) {
        *storage_state =
            slot.state == COPOOL_SLOT_FRESH ? MCO_STORAGE_FRESH
                                            : MCO_STORAGE_REUSABLE;
        return slot.ptr;
    }

    void* ptr = NULL;
    if (arena_alloc(fixture->arena, &ptr, 1) != 1) {
        return NULL;
    }
    *storage_state = MCO_STORAGE_FRESH;
    return ptr;
}

static void _coro_dealloc_cb(
    void*             ptr,
    size_t            size,
    void*             allocator_data,
    mco_storage_state storage_state) {
    (void)size;
    _coro_fixture_t* fixture = (_coro_fixture_t*)allocator_data;

    atomic_fetch_add(&fixture->dealloc_calls, 1);
    if (storage_state == MCO_STORAGE_FRESH) {
        arena_free(fixture->arena, &ptr, 1);
        return;
    }
    ASSERT(storage_state == MCO_STORAGE_REUSABLE);
    copool_slot_t slot = {.ptr = ptr, .state = COPOOL_SLOT_REUSABLE};
    if (fixture->local_pool &&
        copool_local_put(fixture->local_pool, &slot, 1) == 1) {
        return;
    }
    if (fixture->shared_pool &&
        copool_shared_put(fixture->shared_pool, &slot, 1) == 1) {
        return;
    }
    arena_free(fixture->arena, &ptr, 1);
}

static void _coro_fixture_init(_coro_fixture_t* fixture) {
    fixture->arena       = NULL;
    fixture->local_pool  = NULL;
    fixture->shared_pool = NULL;
    atomic_init(&fixture->alloc_calls, 0);
    atomic_init(&fixture->dealloc_calls, 0);
}

static int _coro_fixture_create(
    _coro_fixture_t* fixture,
    size_t slot_size,
    int cached) {
    fixture->arena = arena_create(slot_size);
    if (!fixture->arena) {
        return -1;
    }
    if (!cached) {
        return 0;
    }

    fixture->local_pool  = copool_local_create(0);
    fixture->shared_pool = copool_shared_create();
    if (!fixture->local_pool || !fixture->shared_pool) {
        copool_local_destroy(fixture->local_pool);
        copool_shared_destroy(fixture->shared_pool);
        arena_destroy(fixture->arena);
        fixture->local_pool  = NULL;
        fixture->shared_pool = NULL;
        fixture->arena       = NULL;
        return -1;
    }
    return 0;
}

static void _coro_fixture_destroy(_coro_fixture_t* fixture) {
    copool_local_destroy(fixture->local_pool);
    copool_shared_destroy(fixture->shared_pool);
    arena_destroy(fixture->arena);
    fixture->local_pool  = NULL;
    fixture->shared_pool = NULL;
    fixture->arena       = NULL;
}

#if defined(TEST_MCO_WINDOWS_ASM)
static void* _ordered_alloc_cb(
    size_t             size,
    void*              allocator_data,
    mco_storage_state* storage_state) {
    (void)size;
    _ordered_fixture_t* fixture = (_ordered_fixture_t*)allocator_data;

    if (fixture->next_slot >= 2) {
        return NULL;
    }
    *storage_state = MCO_STORAGE_FRESH;
    return fixture->slots[fixture->next_slot++];
}

static void _ordered_dealloc_cb(
    void*             ptr,
    size_t            size,
    void*             allocator_data,
    mco_storage_state storage_state) {
    (void)ptr;
    (void)size;
    (void)allocator_data;
    (void)storage_state;
}

static void _fresh_child_entry(mco_coro* co) {
    _fresh_child_ctx_t* ctx = (_fresh_child_ctx_t*)mco_get_user_data(co);
    NT_TIB*            tib = (NT_TIB*)NtCurrentTeb();
    mco_desc           child_desc = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_coro*          child      = NULL;

    child_desc.alloc_cb       = _ordered_alloc_cb;
    child_desc.dealloc_cb     = _ordered_dealloc_cb;
    child_desc.allocator_data = ctx->fixture;

    ctx->stack_base_before  = tib->StackBase;
    ctx->stack_limit_before = tib->StackLimit;
    ctx->create_result      = mco_create(&child, &child_desc);
    ctx->stack_base_after   = tib->StackBase;
    ctx->stack_limit_after  = tib->StackLimit;
    if (ctx->create_result == MCO_SUCCESS) {
        ctx->destroy_result = mco_destroy(child);
    }
}
#endif

static int _migration_signal(_migration_ctx_t* ctx, int state) {
    int result = 0;

    if (mtx_lock(&ctx->lock) != thrd_success) {
        return -1;
    }
    atomic_fetch_or(&ctx->state, state);
    if (cnd_broadcast(&ctx->condition) != thrd_success) {
        result = -1;
    }
    if (mtx_unlock(&ctx->lock) != thrd_success) {
        result = -1;
    }
    return result;
}

static int _migration_wait(_migration_ctx_t* ctx, int state) {
    int result = 0;

    if (mtx_lock(&ctx->lock) != thrd_success) {
        return -1;
    }
    while ((atomic_load(&ctx->state) & state) != state) {
        if (cnd_wait(&ctx->condition, &ctx->lock) != thrd_success) {
            result = -1;
            break;
        }
    }
    if (mtx_unlock(&ctx->lock) != thrd_success) {
        result = -1;
    }
    return result;
}

static int _migration_first_thread(void* arg) {
    _migration_ctx_t* ctx = (_migration_ctx_t*)arg;
    mco_result        result;

    if (_migration_signal(ctx, MIGRATION_FIRST_READY) != 0 ||
        _migration_wait(ctx, MIGRATION_FIRST_GO) != 0) {
        return -1;
    }
    result = mco_resume((mco_coro*)atomic_load(&ctx->co));
    if (_migration_signal(ctx, MIGRATION_FIRST_DONE) != 0 ||
        _migration_wait(ctx, MIGRATION_SECOND_DONE) != 0) {
        return -1;
    }
    return result == MCO_SUCCESS ? 0 : -1;
}

static int _migration_second_thread(void* arg) {
    _migration_ctx_t* ctx = (_migration_ctx_t*)arg;
    mco_result        result;

    if (_migration_signal(ctx, MIGRATION_SECOND_READY) != 0 ||
        _migration_wait(ctx, MIGRATION_SECOND_GO) != 0) {
        return -1;
    }
    result = mco_resume((mco_coro*)atomic_load(&ctx->co));
    if (_migration_signal(ctx, MIGRATION_SECOND_DONE) != 0) {
        return -1;
    }
    return result == MCO_SUCCESS ? 0 : -1;
}

#if defined(TEST_MCO_WINDOWS_ASM)
static void _assert_windows_page(
    const uint8_t* ptr,
    DWORD          expected_state,
    DWORD          expected_base_protection,
    int            expected_guard) {
    MEMORY_BASIC_INFORMATION info;

    ASSERT(VirtualQuery(ptr, &info, sizeof(info)) == sizeof(info));
    ASSERT(info.State == expected_state);
    ASSERT((info.Protect & 0xffU) == expected_base_protection);
    ASSERT(((info.Protect & PAGE_GUARD) != 0) == expected_guard);
}

static void _assert_initial_stack_layout(const mco_coro* co) {
    size_t         page_size  = platform_vmem_page_size();
    const uint8_t* stack_low  = (const uint8_t*)co->stack_base;
    const uint8_t* stack_high = stack_low + co->stack_size;

    ASSERT(page_size > 0);
    ASSERT(co->stack_size >= page_size * 3U);
    _assert_windows_page(stack_low - 1U, MEM_COMMIT, PAGE_READWRITE, 0);
    _assert_windows_page(stack_low, MEM_RESERVE, 0, 0);
    _assert_windows_page(stack_high - page_size * 3U, MEM_RESERVE, 0, 0);
    _assert_windows_page(
        stack_high - page_size * 2U,
        MEM_COMMIT,
        PAGE_READWRITE,
        1);
    _assert_windows_page(stack_high - page_size, MEM_COMMIT, PAGE_READWRITE, 0);
}

#endif

static void test_stack_layout(void) {
    mco_desc  desc = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_coro* co   = NULL;

    ASSERT(mco_create(&co, &desc) == MCO_SUCCESS);
    ASSERT(co->stack_base != NULL);
    ASSERT(co->stack_size > 0);
#if defined(TEST_MCO_WINDOWS_ASM)
    ASSERT(co->stack_size == desc.stack_size);
#endif
    ASSERT(mco_destroy(co) == MCO_SUCCESS);
}

static void test_init_accessible_storage(void) {
    mco_desc  desc = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_coro* co   = (mco_coro*)malloc(desc.coro_size);

    ASSERT(co != NULL);
    ASSERT(mco_init(co, &desc) == MCO_SUCCESS);
    ASSERT(mco_resume(co) == MCO_SUCCESS);
    ASSERT(mco_status(co) == MCO_DEAD);
    ASSERT(mco_uninit(co) == MCO_SUCCESS);
    free(co);
}

static void test_create_rejects_invalid_desc_before_allocation(void) {
    mco_desc        desc    = mco_desc_init(_empty_entry, 128U * 1024U);
    _coro_fixture_t fixture = {0};
    mco_coro*       co      = NULL;

    _coro_fixture_init(&fixture);
    ASSERT(_coro_fixture_create(&fixture, desc.coro_size, 1) == 0);
    desc.func           = NULL;
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;

    ASSERT(mco_create(&co, &desc) == MCO_INVALID_ARGUMENTS);
    ASSERT(co == NULL);
    ASSERT(atomic_load(&fixture.alloc_calls) == 0);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 0);

    _coro_fixture_destroy(&fixture);
}

static void test_minicoro_prepares_fresh_arena_slot(void) {
    mco_desc        desc = mco_desc_init(_empty_entry, 128U * 1024U);
    _arena_fixture_t fixture = {
        .arena = arena_create(desc.coro_size),
    };
    ASSERT(fixture.arena != NULL);

    desc.alloc_cb       = _arena_alloc_cb;
    desc.dealloc_cb     = _arena_dealloc_cb;
    desc.allocator_data = &fixture;

    mco_coro* co = NULL;
    ASSERT(mco_create(&co, &desc) == MCO_SUCCESS);
    ASSERT(co != NULL);
    ASSERT(mco_resume(co) == MCO_SUCCESS);
    ASSERT(mco_status(co) == MCO_DEAD);
    ASSERT(mco_destroy(co) == MCO_SUCCESS);

    arena_destroy(fixture.arena);
}

#if defined(TEST_MCO_WINDOWS_ASM)
static void test_create_rejects_invalid_fresh_layout(void) {
    mco_desc        desc = mco_desc_init(_empty_entry, 128U * 1024U);
    _arena_fixture_t fixture = {
        .arena = arena_create(desc.coro_size),
    };
    ASSERT(fixture.arena != NULL);

    desc.stack_size -= 16U;
    desc.alloc_cb       = _arena_alloc_cb;
    desc.dealloc_cb     = _arena_dealloc_cb;
    desc.allocator_data = &fixture;

    mco_coro* co = NULL;
    ASSERT(mco_create(&co, &desc) == MCO_INVALID_ARGUMENTS);
    ASSERT(co == NULL);

    arena_destroy(fixture.arena);
}
#endif

static void test_create_prepared_allocator(void) {
    mco_desc          desc      = mco_desc_init(_empty_entry, 128U * 1024U);
    _coro_fixture_t   fixture   = {0};
    mco_coro*         co = NULL;
    void*             stack_base;
    mco_result        create_result;
    mco_result        destroy_result = MCO_GENERIC_ERROR;
    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(_coro_fixture_create(&fixture, desc.coro_size, 1) == 0);

    create_result = mco_create(&co, &desc);
    ASSERT(create_result == MCO_SUCCESS);
    ASSERT(co != NULL);
    stack_base   = co->stack_base;
#if defined(TEST_MCO_WINDOWS_ASM)
    _assert_initial_stack_layout(co);
#endif
    destroy_result = mco_destroy(co);
    _coro_fixture_destroy(&fixture);

    ASSERT(stack_base != NULL);
    ASSERT(destroy_result == MCO_SUCCESS);
    ASSERT(desc.alloc_cb == _coro_alloc_cb);
    ASSERT(desc.dealloc_cb == _coro_dealloc_cb);
    ASSERT(desc.allocator_data == &fixture);
}

#if defined(TEST_MCO_WINDOWS_ASM)
static void test_initial_teb_state(void) {
    mco_desc          desc      = mco_desc_init(_teb_entry, 128U * 1024U);
    _coro_fixture_t   fixture   = {0};
    _teb_entry_ctx_t  entry = {0};
    mco_desc          create_desc;
    mco_coro*         co;
    uint8_t*          stack_low;
    uint8_t*          stack_high;
    size_t            page_size;

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(_coro_fixture_create(&fixture, desc.coro_size, 1) == 0);

    create_desc           = desc;
    create_desc.user_data = &entry;
    ASSERT(mco_create(&co, &create_desc) == MCO_SUCCESS);
    stack_low  = (uint8_t*)co->stack_base;
    stack_high = stack_low + desc.stack_size;
    page_size  = platform_vmem_page_size();
    ASSERT(page_size > 0);
    ASSERT(mco_resume(co) == MCO_SUCCESS);
    ASSERT(entry.stack_base == stack_high);
#if defined(TEST_MCO_ASAN)
    ASSERT((uintptr_t)entry.stack_limit > (uintptr_t)stack_low);
    ASSERT(
        (uintptr_t)entry.stack_limit <=
        (uintptr_t)(stack_high - page_size));
#else
    ASSERT(entry.stack_limit == stack_high - page_size);
#endif
    ASSERT(mco_destroy(co) == MCO_SUCCESS);

    _coro_fixture_destroy(&fixture);
}

static void test_fresh_child_creation_preserves_parent_teb(void) {
    mco_desc          desc = mco_desc_init(_fresh_child_entry, 128U * 1024U);
    _ordered_fixture_t fixture = {
        .arena = arena_create(desc.coro_size),
    };
    _fresh_child_ctx_t child_ctx = {
        .fixture        = &fixture,
        .create_result  = MCO_GENERIC_ERROR,
        .destroy_result = MCO_GENERIC_ERROR,
    };
    mco_coro* parent = NULL;

    ASSERT(fixture.arena != NULL);
    ASSERT(arena_alloc(fixture.arena, fixture.slots, 2) == 2);
    if ((uintptr_t)fixture.slots[0] > (uintptr_t)fixture.slots[1]) {
        void* slot       = fixture.slots[0];
        fixture.slots[0] = fixture.slots[1];
        fixture.slots[1] = slot;
    }
    desc.user_data      = &child_ctx;
    desc.alloc_cb       = _ordered_alloc_cb;
    desc.dealloc_cb     = _ordered_dealloc_cb;
    desc.allocator_data = &fixture;

    ASSERT(mco_create(&parent, &desc) == MCO_SUCCESS);
    ASSERT(mco_resume(parent) == MCO_SUCCESS);
    ASSERT(mco_status(parent) == MCO_DEAD);
    ASSERT(mco_destroy(parent) == MCO_SUCCESS);
    arena_free(fixture.arena, fixture.slots, 2);
    arena_destroy(fixture.arena);

    ASSERT(child_ctx.create_result == MCO_SUCCESS);
    ASSERT(child_ctx.destroy_result == MCO_SUCCESS);
    ASSERT(child_ctx.stack_base_before == child_ctx.stack_base_after);
    ASSERT(child_ctx.stack_limit_before == child_ctx.stack_limit_after);
}
#endif

static void test_create_destroy_reuse(void) {
    mco_desc          desc      = mco_desc_init(_empty_entry, 128U * 1024U);
    _coro_fixture_t   fixture        = {0};
    mco_coro*         first          = NULL;
    mco_coro*         second         = NULL;
    void*             first_ptr      = NULL;
    mco_result        first_create   = MCO_GENERIC_ERROR;
    mco_result        first_destroy  = MCO_GENERIC_ERROR;
    mco_result        second_create  = MCO_GENERIC_ERROR;
    mco_result        second_destroy = MCO_GENERIC_ERROR;

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(_coro_fixture_create(&fixture, desc.coro_size, 1) == 0);

    first_create = mco_create(&first, &desc);
    if (first_create == MCO_SUCCESS) {
        first_ptr     = first;
        first_destroy = mco_destroy(first);
    }
    if (first_destroy == MCO_SUCCESS) {
        second_create = mco_create(&second, &desc);
    }
    if (second_create == MCO_SUCCESS) {
        second_destroy = mco_destroy(second);
    }
    _coro_fixture_destroy(&fixture);

    ASSERT(first_create == MCO_SUCCESS);
    ASSERT(first_destroy == MCO_SUCCESS);
    ASSERT(second_create == MCO_SUCCESS);
    ASSERT(second_destroy == MCO_SUCCESS);
    ASSERT(second == first_ptr);
}

static void test_cross_thread_stack_migration(void) {
    mco_desc          desc      = mco_desc_init(_migration_entry, 128U * 1024U);
    _coro_fixture_t   fixture = {0};
    _migration_ctx_t  migration;
    mco_desc          create_desc;
    mco_coro*         co = NULL;
    thrd_t            first_thread;
    thrd_t            second_thread;
    int               first_result  = -1;
    int               second_result = -1;

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(_coro_fixture_create(&fixture, desc.coro_size, 1) == 0);

    atomic_init(&migration.co, 0);
    ASSERT(mtx_init(&migration.lock, mtx_plain) == thrd_success);
    ASSERT(cnd_init(&migration.condition) == thrd_success);
    atomic_init(&migration.state, 0);
    atomic_init(&migration.entry_error, 0);
    atomic_init(&migration.first_tid, 0);
    atomic_init(&migration.second_tid, 0);
    atomic_init(&migration.checksum, 0);

    create_desc           = desc;
    create_desc.user_data = &migration;
    ASSERT(mco_create(&co, &create_desc) == MCO_SUCCESS);
    ASSERT(co != NULL);
    atomic_store(&migration.co, (uintptr_t)co);
    ASSERT(
        thrd_create(&first_thread, _migration_first_thread, &migration) ==
        thrd_success);
    ASSERT(
        thrd_create(&second_thread, _migration_second_thread, &migration) ==
        thrd_success);

    ASSERT(
        _migration_wait(
            &migration,
            MIGRATION_FIRST_READY | MIGRATION_SECOND_READY) == 0);
    ASSERT(_migration_signal(&migration, MIGRATION_FIRST_GO) == 0);
    ASSERT(_migration_wait(&migration, MIGRATION_FIRST_DONE) == 0);
    ASSERT(mco_status(co) == MCO_SUSPENDED);
    ASSERT(atomic_load(&migration.first_tid) != 0);
    ASSERT(atomic_load(&migration.second_tid) == 0);

    ASSERT(_migration_signal(&migration, MIGRATION_SECOND_GO) == 0);
    ASSERT(_migration_wait(&migration, MIGRATION_SECOND_DONE) == 0);
    ASSERT(mco_status(co) == MCO_DEAD);
    ASSERT(thrd_join(first_thread, &first_result) == thrd_success);
    ASSERT(thrd_join(second_thread, &second_result) == thrd_success);
    ASSERT(first_result == 0);
    ASSERT(second_result == 0);
    ASSERT(atomic_load(&migration.entry_error) == 0);
    ASSERT(atomic_load(&migration.checksum) != 0);
    ASSERT(atomic_load(&migration.second_tid) != 0);
    ASSERT(
        atomic_load(&migration.first_tid) !=
        atomic_load(&migration.second_tid));
    ASSERT(mco_destroy(co) == MCO_SUCCESS);

    cnd_destroy(&migration.condition);
    mtx_destroy(&migration.lock);
    _coro_fixture_destroy(&fixture);
    ASSERT(atomic_load(&fixture.alloc_calls) == 1);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 1);
}

static void test_reusable_stack_limit(void) {
    mco_desc          desc      = mco_desc_init(_stack_entry, 128U * 1024U);
    _coro_fixture_t   fixture = {0};
    _stack_entry_ctx_t deep = {
        .seed = 0x21U,
        .deep = 1,
    };
#if defined(TEST_MCO_WINDOWS_ASM) && defined(TEST_MCO_ASAN)
    deep.allocate = 1;
#endif
    _stack_entry_ctx_t shallow = {
        .seed = 0x61U,
        .deep = 0,
    };
    mco_desc  create_desc;
    mco_coro* first  = NULL;
    mco_coro* second = NULL;
    void*     first_slot;
#if defined(TEST_MCO_WINDOWS_ASM)
    void*     initial_limit;
#endif

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(_coro_fixture_create(&fixture, desc.coro_size, 1) == 0);

    create_desc           = desc;
    create_desc.user_data = &deep;
    ASSERT(mco_create(&first, &create_desc) == MCO_SUCCESS);
    ASSERT(first != NULL);
    first_slot = first;
#if defined(TEST_MCO_WINDOWS_ASM)
    initial_limit = (uint8_t*)first->stack_base + first->stack_size -
                    platform_vmem_page_size();
#endif
    ASSERT(mco_resume(first) == MCO_SUCCESS);
    ASSERT(mco_status(first) == MCO_DEAD);
    ASSERT(deep.checksum != 0);
#if defined(TEST_MCO_WINDOWS_ASM)
    ASSERT(deep.stack_limit != NULL);
    ASSERT((uintptr_t)deep.stack_limit < (uintptr_t)initial_limit);
#endif
    ASSERT(mco_destroy(first) == MCO_SUCCESS);

    create_desc.user_data = &shallow;
    ASSERT(mco_create(&second, &create_desc) == MCO_SUCCESS);
    ASSERT(second == first_slot);
    ASSERT(mco_resume(second) == MCO_SUCCESS);
    ASSERT(mco_status(second) == MCO_DEAD);
    ASSERT(shallow.checksum != 0);
#if defined(TEST_MCO_WINDOWS_ASM)
    ASSERT(shallow.stack_limit == deep.stack_limit);
#endif
    ASSERT(mco_destroy(second) == MCO_SUCCESS);

    _coro_fixture_destroy(&fixture);
    ASSERT(atomic_load(&fixture.alloc_calls) == 2);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 2);
}

#if defined(TEST_MCO_WINDOWS_ASM)
static void test_arena_spill_restores_initial_stack(void) {
    mco_desc          desc      = mco_desc_init(_stack_entry, 128U * 1024U);
    _coro_fixture_t   fixture = {0};

    _stack_entry_ctx_t entry = {
        .seed = 0x31U,
        .deep = 1,
    };
    mco_desc  create_desc;
    mco_coro* first  = NULL;
    mco_coro* second = NULL;
    void*     first_slot;
    void*     initial_limit;

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(_coro_fixture_create(&fixture, desc.coro_size, 0) == 0);

    create_desc           = desc;
    create_desc.user_data = &entry;
    ASSERT(mco_create(&first, &create_desc) == MCO_SUCCESS);
    ASSERT(first != NULL);
    first_slot    = first;
    initial_limit = (uint8_t*)first->stack_base + first->stack_size -
                    platform_vmem_page_size();
    ASSERT(initial_limit != NULL);
    ASSERT(mco_resume(first) == MCO_SUCCESS);
    ASSERT(mco_status(first) == MCO_DEAD);
    ASSERT(entry.checksum != 0);
    ASSERT((uintptr_t)entry.stack_limit < (uintptr_t)initial_limit);
    ASSERT(mco_destroy(first) == MCO_SUCCESS);
    _assert_windows_page((const uint8_t*)first_slot, MEM_RESERVE, 0, 0);
    _assert_windows_page((const uint8_t*)initial_limit, MEM_RESERVE, 0, 0);

    entry.checksum = 0;
    entry.deep     = 0;
    entry.stack_limit = NULL;
    ASSERT(mco_create(&second, &create_desc) == MCO_SUCCESS);
    ASSERT(second == first_slot);
    _assert_initial_stack_layout(second);
    ASSERT(mco_resume(second) == MCO_SUCCESS);
    ASSERT(mco_status(second) == MCO_DEAD);
    ASSERT(entry.checksum != 0);
    ASSERT(entry.stack_limit == initial_limit);
    ASSERT(mco_destroy(second) == MCO_SUCCESS);

    _coro_fixture_destroy(&fixture);
    ASSERT(atomic_load(&fixture.alloc_calls) == 2);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 2);
}

#endif

#if defined(TEST_MCO_WINDOWS_SEH) && !defined(TEST_MCO_ASAN)
static void test_stack_overflow_stops_before_metadata(void) {
    mco_desc          desc      = mco_desc_init(_overflow_entry, 128U * 1024U);
    _coro_fixture_t   fixture  = {0};
    _overflow_ctx_t   overflow = {
          .recurse        = _overflow_recurse,
          .sentinel_value = UINT64_C(0x5a3cc35aa55ac33c),
          .keep_recursing = 1,
          .exception_code = 0,
          .checksum       = 0,
          .reset_result   = 0,
          .returned       = 0,
          .calls          = 0,
    };
    mco_desc  create_desc;
    mco_coro* co = NULL;
    uint8_t*  metadata_end;
    uint8_t*  storage_end;
    size_t    page_size;

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(_coro_fixture_create(&fixture, desc.coro_size, 1) == 0);

    create_desc           = desc;
    create_desc.user_data = &overflow;
    ASSERT(mco_create(&co, &create_desc) == MCO_SUCCESS);
    ASSERT(co != NULL);
    page_size    = platform_vmem_page_size();
    ASSERT(page_size > 0);
    metadata_end = (uint8_t*)co->stack_base;
    storage_end  = (uint8_t*)co->storage + co->storage_size;
    ASSERT(
        (uintptr_t)storage_end <=
        (uintptr_t)metadata_end - sizeof(*overflow.sentinel));
    overflow.sentinel  = (uint64_t*)(metadata_end - sizeof(*overflow.sentinel));
    *overflow.sentinel = overflow.sentinel_value;

    ASSERT(mco_resume(co) == MCO_SUCCESS);
    ASSERT(mco_status(co) == MCO_DEAD);
    ASSERT(overflow.exception_code == EXCEPTION_STACK_OVERFLOW);
    ASSERT(overflow.reset_result != 0);
    ASSERT(overflow.returned != 0);
    ASSERT(overflow.calls > 0);
    ASSERT(*overflow.sentinel == overflow.sentinel_value);
    ASSERT(mco_destroy(co) == MCO_SUCCESS);
    ASSERT(*overflow.sentinel == overflow.sentinel_value);

    _coro_fixture_destroy(&fixture);
    ASSERT(atomic_load(&fixture.alloc_calls) == 1);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 1);
}
#endif

int main(void) {
    test_stack_layout();
    test_init_accessible_storage();
    test_create_rejects_invalid_desc_before_allocation();
    test_minicoro_prepares_fresh_arena_slot();
#if defined(TEST_MCO_WINDOWS_ASM)
    test_create_rejects_invalid_fresh_layout();
#endif
    test_create_prepared_allocator();
#if defined(TEST_MCO_WINDOWS_ASM)
    test_initial_teb_state();
    test_fresh_child_creation_preserves_parent_teb();
#endif
    test_create_destroy_reuse();
    test_cross_thread_stack_migration();
    test_reusable_stack_limit();
#if defined(TEST_MCO_WINDOWS_ASM)
    test_arena_spill_restores_initial_stack();
#endif
#if defined(TEST_MCO_WINDOWS_SEH) && !defined(TEST_MCO_ASAN)
    test_stack_overflow_stops_before_metadata();
#endif
    return 0;
}
