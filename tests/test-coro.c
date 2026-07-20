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
#include "platform/platform-info.h"
#include "platform/platform-vmem.h"
#include "runtime/copool.h"
#include "runtime/coro.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdint.h>
#include <threads.h>

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

#if defined(TEST_MCO_WINDOWS_SEH) && !defined(TEST_MCO_ASAN)
#include <malloc.h>
#endif

typedef struct {
    void*  ptr;
    size_t reserve_size;
    int    alloc_calls;
    int    dealloc_calls;
    int    release_result;
} _vmem_alloc_ctx_t;

typedef struct {
    copool_t*  pool;
    atomic_int alloc_calls;
    atomic_int dealloc_calls;
} _coro_fixture_t;

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
} _stack_entry_ctx_t;

#if defined(TEST_MCO_WINDOWS_ASM)
typedef struct {
    void* stack_base;
    void* stack_limit;
} _teb_entry_ctx_t;
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

static void _count_entry(mco_coro* co) {
    int* entry_calls = (int*)mco_get_user_data(co);

    (*entry_calls)++;
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

static void* _vmem_alloc(size_t size, void* allocator_data) {
    _vmem_alloc_ctx_t* ctx       = (_vmem_alloc_ctx_t*)allocator_data;
    size_t             page_size = platform_vmem_page_size();
    size_t             reserve_size;

    ctx->alloc_calls++;
    if (page_size == 0 || size > SIZE_MAX - (page_size - 1U)) {
        return NULL;
    }
    reserve_size = size + (page_size - size % page_size) % page_size;
    ctx->ptr     = platform_vmem_reserve(reserve_size);
    if (ctx->ptr == NULL) {
        return NULL;
    }
    ctx->reserve_size = reserve_size;
    if (platform_vmem_commit(ctx->ptr, reserve_size) != 0) {
        (void)platform_vmem_release(ctx->ptr, reserve_size);
        ctx->ptr          = NULL;
        ctx->reserve_size = 0;
        return NULL;
    }
    return ctx->ptr;
}

static void _vmem_dealloc(void* ptr, size_t size, void* allocator_data) {
    _vmem_alloc_ctx_t* ctx = (_vmem_alloc_ctx_t*)allocator_data;

    (void)size;
    ctx->dealloc_calls++;
    ctx->release_result = platform_vmem_release(ptr, ctx->reserve_size);
    ctx->ptr            = NULL;
}

static void* _coro_alloc_cb(size_t size, void* allocator_data) {
    _coro_fixture_t* fixture = (_coro_fixture_t*)allocator_data;

    atomic_fetch_add(&fixture->alloc_calls, 1);
    return copool_alloc(fixture->pool, NULL, size);
}

static void _coro_dealloc_cb(void* ptr, size_t size, void* allocator_data) {
    _coro_fixture_t* fixture = (_coro_fixture_t*)allocator_data;

    atomic_fetch_add(&fixture->dealloc_calls, 1);
    copool_free(fixture->pool, NULL, ptr, size);
}

static void _coro_fixture_init(_coro_fixture_t* fixture) {
    fixture->pool = NULL;
    atomic_init(&fixture->alloc_calls, 0);
    atomic_init(&fixture->dealloc_calls, 0);
}

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

static void
_assert_initial_stack_layout(const void* ptr, const mco_desc* desc) {
    size_t         page_size    = platform_vmem_page_size();
    size_t         stack_offset = mco_desc_stack_offset(desc);
    const uint8_t* slot         = (const uint8_t*)ptr;
    const uint8_t* stack_low    = slot + stack_offset;
    const uint8_t* stack_high   = stack_low + desc->stack_size;

    ASSERT(page_size > 0);
    ASSERT(stack_offset >= page_size);
    ASSERT(desc->stack_size >= page_size * 3U);
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

static void test_stack_offset(void) {
    mco_desc  desc = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_coro* co   = NULL;
    size_t    offset;

    ASSERT(mco_create(&co, &desc) == MCO_SUCCESS);
    offset = mco_desc_stack_offset(&desc);
#if defined(TEST_MCO_WINDOWS_ASM)
    size_t page_size = platform_vmem_page_size();

    ASSERT(page_size > 0);
    ASSERT(offset != 0);
    ASSERT(offset % page_size == 0);
    ASSERT(offset >= page_size);
    ASSERT(offset <= desc.coro_size);
    ASSERT(desc.stack_size <= desc.coro_size - offset);
    ASSERT((uint8_t*)co->stack_base == (uint8_t*)co + offset);
    ASSERT(co->stack_size == desc.stack_size);
#elif defined(TEST_MCO_WINDOWS_FIBER)
    ASSERT(offset == 0);
#else
    ASSERT(offset != 0);
    ASSERT(offset <= desc.coro_size);
    ASSERT(desc.stack_size <= desc.coro_size - offset);
    ASSERT((uint8_t*)co->stack_base == (uint8_t*)co + offset);
    ASSERT(co->stack_size == desc.stack_size);
#endif
    ASSERT(mco_destroy(co) == MCO_SUCCESS);
}

static void test_create_ordinary_page_aligned_allocator(void) {
    _vmem_alloc_ctx_t alloc_ctx = {0};
    mco_desc          desc      = mco_desc_init(_empty_entry, 32U * 1024U);
    mco_coro*         co        = NULL;
    void*             stack_base;
    mco_result        create_result;
    mco_result        destroy_result = MCO_GENERIC_ERROR;

    desc.alloc_cb       = _vmem_alloc;
    desc.dealloc_cb     = _vmem_dealloc;
    desc.allocator_data = &alloc_ctx;
    create_result       = coro_create(&co, &desc);
    ASSERT(create_result == MCO_SUCCESS);
    ASSERT(co != NULL);
    ASSERT(co == alloc_ctx.ptr);
    stack_base     = co->stack_base;
    destroy_result = coro_destroy(co);

    ASSERT(stack_base != NULL);
    ASSERT(destroy_result == MCO_SUCCESS);
    ASSERT(alloc_ctx.alloc_calls == 1);
    ASSERT(alloc_ctx.dealloc_calls == 1);
    ASSERT(alloc_ctx.release_result == 0);
    ASSERT(alloc_ctx.ptr == NULL);
}

static void test_alloc_ctx_rejects_descriptor_mutation(void) {
    mco_desc          desc      = mco_desc_init(_empty_entry, 128U * 1024U);
    _coro_fixture_t   fixture   = {0};
    coro_alloc_ctx_t  alloc_ctx = {0};
    copool_slot_ops_t ops;
    mco_coro*         co    = NULL;
    void (*func)(mco_coro*) = desc.func;
    size_t storage_size     = desc.storage_size;
    size_t coro_size        = desc.coro_size;
    size_t stack_size       = desc.stack_size;

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == 0);
    ops          = coro_get_slot_ops(&alloc_ctx);
    fixture.pool = copool_create(desc.coro_size, COPOOL_CACHE_CAP, &ops);
    ASSERT(fixture.pool != NULL);

    desc.func = _count_entry;
    ASSERT(coro_create(&co, &desc) == MCO_INVALID_ARGUMENTS);
    ASSERT(co == NULL);
    desc.func = func;

    desc.storage_size++;
    ASSERT(coro_create(&co, &desc) == MCO_INVALID_ARGUMENTS);
    ASSERT(co == NULL);
    desc.storage_size = storage_size;

    desc.coro_size++;
    ASSERT(coro_create(&co, &desc) == MCO_INVALID_ARGUMENTS);
    ASSERT(co == NULL);
    desc.coro_size = coro_size;

    desc.stack_size--;
    ASSERT(coro_create(&co, &desc) == MCO_INVALID_ARGUMENTS);
    ASSERT(co == NULL);
    desc.stack_size = stack_size;

    ASSERT(atomic_load(&fixture.alloc_calls) == 0);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 0);

    copool_destroy(fixture.pool);
    fixture.pool = NULL;
    coro_alloc_ctx_deinit(&alloc_ctx);
}

static void test_alloc_ctx_rejects_double_init(void) {
    mco_desc         desc      = mco_desc_init(_empty_entry, 128U * 1024U);
    coro_alloc_ctx_t alloc_ctx = {0};
    coro_alloc_ctx_t other_ctx = {0};
    void* (*alloc_cb)(size_t, void*)         = desc.alloc_cb;
    void (*dealloc_cb)(void*, size_t, void*) = desc.dealloc_cb;
    void* allocator_data                     = desc.allocator_data;

    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == 0);
    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == -1);
    ASSERT(coro_alloc_ctx_init(&other_ctx, &desc) == -1);
    coro_alloc_ctx_deinit(&alloc_ctx);
    ASSERT(desc.alloc_cb == alloc_cb);
    ASSERT(desc.dealloc_cb == dealloc_cb);
    ASSERT(desc.allocator_data == allocator_data);
    coro_alloc_ctx_deinit(&alloc_ctx);
    ASSERT(desc.alloc_cb == alloc_cb);
    ASSERT(desc.dealloc_cb == dealloc_cb);
    ASSERT(desc.allocator_data == allocator_data);
}

static void test_create_prepared_allocator(void) {
    mco_desc          desc      = mco_desc_init(_empty_entry, 128U * 1024U);
    _coro_fixture_t   fixture   = {0};
    coro_alloc_ctx_t  alloc_ctx = {0};
    copool_slot_ops_t ops;
    mco_coro*         co = NULL;
    void*             stack_base;
    size_t            stack_offset;
    mco_result        create_result;
    mco_result        destroy_result = MCO_GENERIC_ERROR;
    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == 0);
    ops          = coro_get_slot_ops(&alloc_ctx);
    fixture.pool = copool_create(desc.coro_size, COPOOL_CACHE_CAP, &ops);
    ASSERT(fixture.pool != NULL);

    create_result = coro_create(&co, &desc);
    ASSERT(create_result == MCO_SUCCESS);
    ASSERT(co != NULL);
    stack_base   = co->stack_base;
    stack_offset = mco_desc_stack_offset(&desc);
#if defined(TEST_MCO_WINDOWS_ASM)
    _assert_initial_stack_layout(co, &desc);
#endif
    destroy_result = coro_destroy(co);
    copool_destroy(fixture.pool);
    fixture.pool = NULL;
    coro_alloc_ctx_deinit(&alloc_ctx);

#if defined(TEST_MCO_WINDOWS_ASM)
    ASSERT(stack_offset > 0);
#elif defined(TEST_MCO_WINDOWS_FIBER)
    ASSERT(stack_offset == 0);
#endif
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
    coro_alloc_ctx_t  alloc_ctx = {0};
    copool_slot_ops_t ops;
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
    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == 0);
    ops          = coro_get_slot_ops(&alloc_ctx);
    fixture.pool = copool_create(desc.coro_size, COPOOL_CACHE_CAP, &ops);
    ASSERT(fixture.pool != NULL);

    create_desc           = desc;
    create_desc.user_data = &entry;
    ASSERT(coro_create(&co, &create_desc) == MCO_SUCCESS);
    stack_low  = (uint8_t*)co + mco_desc_stack_offset(&desc);
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
    ASSERT(coro_destroy(co) == MCO_SUCCESS);

    copool_destroy(fixture.pool);
    fixture.pool = NULL;
    coro_alloc_ctx_deinit(&alloc_ctx);
}
#endif

static void test_create_destroy_reuse(void) {
    mco_desc          desc      = mco_desc_init(_empty_entry, 128U * 1024U);
    coro_alloc_ctx_t  alloc_ctx = {0};
    copool_slot_ops_t ops;
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
    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == 0);
    ops          = coro_get_slot_ops(&alloc_ctx);
    fixture.pool = copool_create(desc.coro_size, COPOOL_CACHE_CAP, &ops);
    ASSERT(fixture.pool != NULL);

    first_create = coro_create(&first, &desc);
    if (first_create == MCO_SUCCESS) {
        first_ptr     = first;
        first_destroy = coro_destroy(first);
    }
    if (first_destroy == MCO_SUCCESS) {
        second_create = coro_create(&second, &desc);
    }
    if (second_create == MCO_SUCCESS) {
        second_destroy = coro_destroy(second);
    }
    copool_destroy(fixture.pool);
    fixture.pool = NULL;
    coro_alloc_ctx_deinit(&alloc_ctx);

    ASSERT(first_create == MCO_SUCCESS);
    ASSERT(first_destroy == MCO_SUCCESS);
    ASSERT(second_create == MCO_SUCCESS);
    ASSERT(second_destroy == MCO_SUCCESS);
    ASSERT(second == first_ptr);
}

static void test_cross_thread_stack_migration(void) {
    mco_desc          desc      = mco_desc_init(_migration_entry, 128U * 1024U);
    coro_alloc_ctx_t  alloc_ctx = {0};
    copool_slot_ops_t ops;
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
    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == 0);
    ops          = coro_get_slot_ops(&alloc_ctx);
    fixture.pool = copool_create(desc.coro_size, COPOOL_CACHE_CAP, &ops);
    ASSERT(fixture.pool != NULL);

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
    ASSERT(coro_create(&co, &create_desc) == MCO_SUCCESS);
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
    ASSERT(coro_destroy(co) == MCO_SUCCESS);

    cnd_destroy(&migration.condition);
    mtx_destroy(&migration.lock);
    copool_destroy(fixture.pool);
    fixture.pool = NULL;
    coro_alloc_ctx_deinit(&alloc_ctx);
    ASSERT(atomic_load(&fixture.alloc_calls) == 1);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 1);
}

static void test_hot_stack_limit_reuse(void) {
    mco_desc          desc      = mco_desc_init(_stack_entry, 128U * 1024U);
    coro_alloc_ctx_t  alloc_ctx = {0};
    copool_slot_ops_t ops;
    _coro_fixture_t   fixture = {0};
    _stack_entry_ctx_t deep = {
        .seed = 0x21U,
        .deep = 1,
    };
    _stack_entry_ctx_t shallow = {
        .seed = 0x61U,
        .deep = 0,
    };
    mco_desc  create_desc;
    mco_coro* first  = NULL;
    mco_coro* second = NULL;
    void*     first_slot;
    void*     initial_limit;
    void*     grown_limit;

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == 0);
    ops          = coro_get_slot_ops(&alloc_ctx);
    fixture.pool = copool_create(desc.coro_size, COPOOL_CACHE_CAP, &ops);
    ASSERT(fixture.pool != NULL);

    create_desc           = desc;
    create_desc.user_data = &deep;
    ASSERT(coro_create(&first, &create_desc) == MCO_SUCCESS);
    ASSERT(first != NULL);
    first_slot    = first;
    initial_limit = mco_get_stack_limit(first);
    ASSERT(mco_resume(first) == MCO_SUCCESS);
    ASSERT(mco_status(first) == MCO_DEAD);
    ASSERT(deep.checksum != 0);
    grown_limit = mco_get_stack_limit(first);
#if defined(TEST_MCO_WINDOWS_ASM)
    ASSERT(grown_limit != NULL);
    ASSERT((uintptr_t)grown_limit < (uintptr_t)initial_limit);
#else
    ASSERT(initial_limit == NULL);
    ASSERT(grown_limit == NULL);
#endif
    ASSERT(coro_destroy(first) == MCO_SUCCESS);

    create_desc.user_data = &shallow;
    ASSERT(coro_create(&second, &create_desc) == MCO_SUCCESS);
    ASSERT(second == first_slot);
    ASSERT(mco_get_stack_limit(second) == grown_limit);
    ASSERT(mco_resume(second) == MCO_SUCCESS);
    ASSERT(mco_status(second) == MCO_DEAD);
    ASSERT(shallow.checksum != 0);
    ASSERT(coro_destroy(second) == MCO_SUCCESS);

    copool_destroy(fixture.pool);
    fixture.pool = NULL;
    coro_alloc_ctx_deinit(&alloc_ctx);
    ASSERT(atomic_load(&fixture.alloc_calls) == 2);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 2);
}

#if defined(TEST_MCO_WINDOWS_ASM)
static void test_reject_misaligned_hot_stack_limit(void) {
    mco_desc          desc      = mco_desc_init(_empty_entry, 128U * 1024U);
    coro_alloc_ctx_t  alloc_ctx = {0};
    copool_slot_ops_t ops;
    _coro_fixture_t   fixture = {0};
    mco_coro*         first   = NULL;
    mco_coro*         rejected;
    mco_coro*         recovered = NULL;
    void*             first_slot;
    void*             initial_limit;
    void*             misaligned_limit;
    uint8_t*          stack_low;
    uint8_t*          stack_high;
    size_t            page_size;

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == 0);
    ops          = coro_get_slot_ops(&alloc_ctx);
    fixture.pool = copool_create(desc.coro_size, COPOOL_CACHE_CAP, &ops);
    ASSERT(fixture.pool != NULL);

    ASSERT(coro_create(&first, &desc) == MCO_SUCCESS);
    ASSERT(first != NULL);
    first_slot    = first;
    initial_limit = mco_get_stack_limit(first);
    stack_low     = (uint8_t*)first + mco_desc_stack_offset(&desc);
    stack_high    = stack_low + desc.stack_size;
    page_size     = platform_vmem_page_size();
    ASSERT(initial_limit != NULL);
    ASSERT(page_size > 1U);
    misaligned_limit = (uint8_t*)initial_limit - 1U;
    ASSERT((uintptr_t)misaligned_limit > (uintptr_t)stack_low);
    ASSERT((uintptr_t)misaligned_limit < (uintptr_t)stack_high);
    ASSERT((uintptr_t)misaligned_limit % page_size != 0);
    ASSERT(mco_resume(first) == MCO_SUCCESS);
    ASSERT(mco_status(first) == MCO_DEAD);
    mco_set_stack_limit(first, misaligned_limit);
    ASSERT(coro_destroy(first) == MCO_SUCCESS);

    rejected = (mco_coro*)first_slot;
    ASSERT(coro_create(&rejected, &desc) == MCO_MAKE_CONTEXT_ERROR);
    ASSERT(rejected == NULL);
    ASSERT(coro_create(&recovered, &desc) == MCO_SUCCESS);
    ASSERT(recovered == first_slot);
    ASSERT(mco_get_stack_limit(recovered) == initial_limit);
    ASSERT(mco_resume(recovered) == MCO_SUCCESS);
    ASSERT(mco_status(recovered) == MCO_DEAD);
    ASSERT(coro_destroy(recovered) == MCO_SUCCESS);

    copool_destroy(fixture.pool);
    fixture.pool = NULL;
    coro_alloc_ctx_deinit(&alloc_ctx);
    ASSERT(atomic_load(&fixture.alloc_calls) == 3);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 3);
}
#endif

#if defined(TEST_MCO_WINDOWS_SEH) && !defined(TEST_MCO_ASAN)
static void test_stack_overflow_stops_before_metadata(void) {
    mco_desc          desc      = mco_desc_init(_overflow_entry, 128U * 1024U);
    coro_alloc_ctx_t  alloc_ctx = {0};
    copool_slot_ops_t ops;
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
    size_t    stack_offset;

    _coro_fixture_init(&fixture);
    desc.alloc_cb       = _coro_alloc_cb;
    desc.dealloc_cb     = _coro_dealloc_cb;
    desc.allocator_data = &fixture;
    ASSERT(coro_alloc_ctx_init(&alloc_ctx, &desc) == 0);
    ops          = coro_get_slot_ops(&alloc_ctx);
    fixture.pool = copool_create(desc.coro_size, COPOOL_CACHE_CAP, &ops);
    ASSERT(fixture.pool != NULL);

    create_desc           = desc;
    create_desc.user_data = &overflow;
    ASSERT(coro_create(&co, &create_desc) == MCO_SUCCESS);
    ASSERT(co != NULL);
    page_size    = platform_vmem_page_size();
    stack_offset = mco_desc_stack_offset(&desc);
    ASSERT(page_size > 0);
    ASSERT(stack_offset >= page_size);
    metadata_end = (uint8_t*)co + stack_offset;
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
    ASSERT(coro_destroy(co) == MCO_SUCCESS);
    ASSERT(*overflow.sentinel == overflow.sentinel_value);

    copool_destroy(fixture.pool);
    fixture.pool = NULL;
    coro_alloc_ctx_deinit(&alloc_ctx);
    ASSERT(atomic_load(&fixture.alloc_calls) == 1);
    ASSERT(atomic_load(&fixture.dealloc_calls) == 1);
}
#endif

#if defined(TEST_MCO_WINDOWS_ASM)
static void test_stack_offset_lookup_is_cached(void) {
    const int       lookup_count = 4096;
    mco_desc        desc         = mco_desc_init(_empty_entry, 128U * 1024U);
    LARGE_INTEGER   start;
    LARGE_INTEGER   end;
    SYSTEM_INFO     info;
    int64_t         lookup_ticks = INT64_MAX;
    int64_t         system_ticks = INT64_MAX;
    volatile size_t sink         = 0;

    ASSERT(mco_desc_stack_offset(&desc) > 0);
    GetSystemInfo(&info);
    for (int sample = 0; sample < 5; sample++) {
        QueryPerformanceCounter(&start);
        for (int i = 0; i < lookup_count; i++) {
            sink += mco_desc_stack_offset(&desc);
        }
        QueryPerformanceCounter(&end);
        if (end.QuadPart - start.QuadPart < lookup_ticks) {
            lookup_ticks = end.QuadPart - start.QuadPart;
        }

        QueryPerformanceCounter(&start);
        for (int i = 0; i < lookup_count; i++) {
            GetSystemInfo(&info);
            sink += info.dwPageSize;
        }
        QueryPerformanceCounter(&end);
        if (end.QuadPart - start.QuadPart < system_ticks) {
            system_ticks = end.QuadPart - start.QuadPart;
        }
    }

    ASSERT(sink != 0);
    ASSERT(lookup_ticks * 2 < system_ticks);
}
#endif

int main(void) {
    test_stack_offset();
    test_create_ordinary_page_aligned_allocator();
    test_alloc_ctx_rejects_double_init();
    test_alloc_ctx_rejects_descriptor_mutation();
    test_create_prepared_allocator();
#if defined(TEST_MCO_WINDOWS_ASM)
    test_initial_teb_state();
#endif
    test_create_destroy_reuse();
    test_cross_thread_stack_migration();
    test_hot_stack_limit_reuse();
#if defined(TEST_MCO_WINDOWS_ASM)
    test_reject_misaligned_hot_stack_limit();
#endif
#if defined(TEST_MCO_WINDOWS_SEH) && !defined(TEST_MCO_ASAN)
    test_stack_overflow_stops_before_metadata();
#endif
#if defined(TEST_MCO_WINDOWS_ASM)
    test_stack_offset_lookup_is_cached();
#endif
    return 0;
}
