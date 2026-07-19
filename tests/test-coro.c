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
#include "platform/platform-vmem.h"

#include "runtime/minicoro/minicoro.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(MCO_USE_FIBERS)
#define TEST_MCO_WINDOWS_FIBER 1
#elif defined(_WIN32) && !defined(MCO_USE_FIBERS) && \
      !defined(MCO_USE_UCONTEXT) && !defined(MCO_USE_ASM) && \
      !defined(MCO_USE_ASYNCIFY) && \
      !defined(__x86_64__) && !defined(_M_X64)
#define TEST_MCO_WINDOWS_FIBER 1
#endif

#if defined(_WIN32) && !defined(MCO_USE_FIBERS) && \
    !defined(MCO_USE_UCONTEXT) && !defined(MCO_USE_ASYNCIFY) && \
    (defined(__x86_64__) || defined(_M_X64))
#define TEST_MCO_WINDOWS_ASM 1
#endif

typedef struct {
    size_t slack_size;
    int    alloc_calls;
} _slack_alloc_ctx_t;

typedef struct {
    void*  raw_ptr;
    void*  returned_ptr;
    void*  dealloc_ptr;
    void*  dealloc_allocator_data;
    size_t requested_size;
    size_t dealloc_size;
    int    alloc_calls;
    int    dealloc_calls;
} _misaligned_alloc_ctx_t;

static void _empty_entry(mco_coro* co) {
    (void)co;
}

static void _count_entry(mco_coro* co) {
    int* entry_calls = (int*)mco_get_user_data(co);

    (*entry_calls)++;
}

static void* _slack_alloc(size_t size, void* allocator_data) {
    _slack_alloc_ctx_t* ctx = (_slack_alloc_ctx_t*)allocator_data;

    ctx->alloc_calls++;
    if (size > SIZE_MAX - ctx->slack_size) {
        return NULL;
    }
    return (void*)calloc(1, size + ctx->slack_size);
}

static void _slack_dealloc(void* ptr, size_t size, void* allocator_data) {
    (void)size;
    (void)allocator_data;
    free(ptr);
}

static void* _misaligned_alloc(size_t size, void* allocator_data) {
    _misaligned_alloc_ctx_t* ctx = (_misaligned_alloc_ctx_t*)allocator_data;
    uint8_t*                 raw;
    size_t                   padding;

    ctx->alloc_calls++;
    ctx->requested_size = size;
    if (size > SIZE_MAX - 32U) {
        return NULL;
    }
    raw = (uint8_t*)calloc(1, size + 32U);
    if (raw == NULL) {
        return NULL;
    }
    padding = (size_t)((16U - (uintptr_t)raw % 16U) % 16U);
    ctx->raw_ptr = raw;
    ctx->returned_ptr = raw + padding + 8U;
    return ctx->returned_ptr;
}

static void _misaligned_dealloc(void* ptr, size_t size, void* allocator_data) {
    _misaligned_alloc_ctx_t* ctx = (_misaligned_alloc_ctx_t*)allocator_data;

    ctx->dealloc_calls++;
    ctx->dealloc_ptr = ptr;
    ctx->dealloc_size = size;
    ctx->dealloc_allocator_data = allocator_data;
    free(ctx->raw_ptr);
    ctx->raw_ptr = NULL;
}

static int _buffer_is_filled(const uint8_t* ptr, size_t size, uint8_t value) {
    for (size_t i = 0; i < size; i++) {
        if (ptr[i] != value) {
            return 0;
        }
    }
    return 1;
}

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

static void test_storage_size_mutation(void) {
    size_t             page_size = platform_vmem_page_size();
    _slack_alloc_ctx_t alloc_ctx = {.slack_size = page_size};
    mco_desc           desc      = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_coro*          co        = NULL;
    mco_result         result;
    int                returned_co;

    ASSERT(page_size > 0);
    ASSERT(desc.storage_size <= SIZE_MAX - page_size);
    desc.alloc_cb = _slack_alloc;
    desc.dealloc_cb = _slack_dealloc;
    desc.allocator_data = &alloc_ctx;
    desc.storage_size += page_size;
    ASSERT(mco_desc_stack_offset(&desc) == 0);

    result = mco_create(&co, &desc);
    returned_co = co != NULL;
    if (co != NULL) {
        ASSERT(mco_destroy(co) == MCO_SUCCESS);
        co = NULL;
    }
    ASSERT(result == MCO_INVALID_ARGUMENTS);
    ASSERT(returned_co == 0);
    ASSERT(co == NULL);
    ASSERT(alloc_ctx.alloc_calls == 0);
}

static void test_stack_limit(void) {
    mco_desc  desc = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_coro* co   = NULL;
    void*     original;

    ASSERT(mco_create(&co, &desc) == MCO_SUCCESS);
    original = mco_get_stack_limit(co);
#if defined(TEST_MCO_WINDOWS_ASM)
    ASSERT(original != NULL);
    void* replacement = (uint8_t*)original + 16;

    mco_set_stack_limit(co, replacement);
    ASSERT(mco_get_stack_limit(co) == replacement);
    mco_set_stack_limit(co, original);
    ASSERT(mco_get_stack_limit(co) == original);
#else
    ASSERT(original == NULL);
    mco_set_stack_limit(co, NULL);
    ASSERT(mco_get_stack_limit(co) == NULL);
#endif
    ASSERT(mco_destroy(co) == MCO_SUCCESS);
}

static void test_stack_size_overflow(void) {
    _slack_alloc_ctx_t alloc_ctx = {0};
    mco_desc           desc      = mco_desc_init(_empty_entry, SIZE_MAX);
    mco_coro*          co        = NULL;
    mco_coro           local_co  = {0};

    ASSERT(desc.coro_size == 0);
    ASSERT(desc.stack_size == 0);
    ASSERT(mco_desc_stack_offset(&desc) == 0);
    local_co.magic_number = 1234U;
    ASSERT(mco_init(&local_co, &desc) == MCO_INVALID_ARGUMENTS);
    ASSERT(local_co.magic_number == 1234U);
    desc.alloc_cb = _slack_alloc;
    desc.dealloc_cb = _slack_dealloc;
    desc.allocator_data = &alloc_ctx;
    ASSERT(mco_create(&co, &desc) == MCO_INVALID_ARGUMENTS);
    ASSERT(co == NULL);
    ASSERT(alloc_ctx.alloc_calls == 0);
}

static void test_allocator_alignment(void) {
    _misaligned_alloc_ctx_t alloc_ctx = {0};
    mco_desc                desc      = mco_desc_init(_count_entry, 128U * 1024U);
    mco_coro*               co        = NULL;
    mco_result              result;
    int                     entry_calls = 0;

    desc.alloc_cb = _misaligned_alloc;
    desc.dealloc_cb = _misaligned_dealloc;
    desc.allocator_data = &alloc_ctx;
    desc.user_data = &entry_calls;
    result = mco_create(&co, &desc);
    ASSERT((uintptr_t)alloc_ctx.returned_ptr % 16U == 8U);
    ASSERT((uintptr_t)alloc_ctx.returned_ptr % sizeof(void*) == 0);
#if defined(TEST_MCO_WINDOWS_FIBER)
    ASSERT(result == MCO_SUCCESS);
    ASSERT(co == alloc_ctx.returned_ptr);
    ASSERT(mco_destroy(co) == MCO_SUCCESS);
    co = NULL;
#else
    if (result == MCO_SUCCESS) {
        ASSERT(mco_destroy(co) == MCO_SUCCESS);
        co = NULL;
    }
    ASSERT(result == MCO_INVALID_ARGUMENTS);
    ASSERT(co == NULL);
#endif
    ASSERT(entry_calls == 0);
    ASSERT(alloc_ctx.alloc_calls == 1);
    ASSERT(alloc_ctx.dealloc_calls == 1);
    ASSERT(alloc_ctx.requested_size == desc.coro_size);
    ASSERT(alloc_ctx.dealloc_ptr == alloc_ctx.returned_ptr);
    ASSERT(alloc_ctx.dealloc_size == desc.coro_size);
    ASSERT(alloc_ctx.dealloc_allocator_data == &alloc_ctx);
    ASSERT(alloc_ctx.raw_ptr == NULL);
}

static void test_init_alignment(void) {
    mco_desc  desc = mco_desc_init(_empty_entry, 128U * 1024U);
    uint8_t*  raw;
    uint8_t*  buffer;
    size_t    padding;
    mco_result result;
    mco_result uninit_result = MCO_SUCCESS;
#if !defined(TEST_MCO_WINDOWS_FIBER)
    int unchanged;
#endif

    ASSERT(desc.coro_size <= SIZE_MAX - 32U);
#if defined(TEST_MCO_WINDOWS_FIBER)
    ASSERT(mco_desc_stack_offset(&desc) == 0);
#else
    ASSERT(mco_desc_stack_offset(&desc) != 0);
#endif
    raw = (uint8_t*)malloc(desc.coro_size + 32U);
    ASSERT(raw != NULL);
    padding = (size_t)((16U - (uintptr_t)raw % 16U) % 16U);
    buffer = raw + padding + 8U;
    ASSERT((uintptr_t)buffer % 16U == 8U);
    ASSERT((uintptr_t)buffer % sizeof(void*) == 0);
    memset(buffer, 0xa5, desc.coro_size);

    result = mco_init((mco_coro*)buffer, &desc);
#if defined(TEST_MCO_WINDOWS_FIBER)
    if (result == MCO_SUCCESS) {
        uninit_result = mco_uninit((mco_coro*)buffer);
    }
    free(raw);
    ASSERT(result == MCO_SUCCESS);
    ASSERT(uninit_result == MCO_SUCCESS);
#else
    unchanged = _buffer_is_filled(buffer, desc.coro_size, 0xa5);
    if (result == MCO_SUCCESS) {
        uninit_result = mco_uninit((mco_coro*)buffer);
    }
    free(raw);
    ASSERT(uninit_result == MCO_SUCCESS);
    ASSERT(result == MCO_INVALID_ARGUMENTS);
    ASSERT(unchanged != 0);
#endif
}

#if defined(TEST_MCO_WINDOWS_ASM)
static void test_stack_size_mutation(void) {
    mco_desc   desc = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_coro*  co   = NULL;
    mco_result result;
    int        returned_co;

    ASSERT(desc.stack_size > 0);
    desc.stack_size--;
    result = mco_create(&co, &desc);
    returned_co = co != NULL;
    if (co != NULL) {
        ASSERT(mco_destroy(co) == MCO_SUCCESS);
        co = NULL;
    }
    ASSERT(result == MCO_INVALID_ARGUMENTS);
    ASSERT(returned_co == 0);
    ASSERT(co == NULL);
    ASSERT(mco_desc_stack_offset(&desc) == 0);
}

static void test_stack_size_layout_overflow(void) {
    size_t             page_size = platform_vmem_page_size();
    size_t             stack_size;
    size_t             stack_offset;
    _slack_alloc_ctx_t alloc_ctx = {0};
    mco_desc           valid_desc = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_desc           desc;
    mco_coro*          co = NULL;

    ASSERT(page_size > 0);
    stack_offset = mco_desc_stack_offset(&valid_desc);
    ASSERT(stack_offset > 0);
    stack_size = SIZE_MAX - SIZE_MAX % page_size;
    ASSERT(stack_size % 16U == 0);
    ASSERT(stack_size % page_size == 0);
    ASSERT(stack_size > SIZE_MAX - stack_offset);
    desc = mco_desc_init(_empty_entry, stack_size);
    ASSERT(desc.coro_size == 0);
    ASSERT(desc.stack_size == 0);
    ASSERT(mco_desc_stack_offset(&desc) == 0);
    desc.alloc_cb = _slack_alloc;
    desc.dealloc_cb = _slack_dealloc;
    desc.allocator_data = &alloc_ctx;
    ASSERT(mco_create(&co, &desc) == MCO_INVALID_ARGUMENTS);
    ASSERT(co == NULL);
    ASSERT(alloc_ctx.alloc_calls == 0);
}
#endif

int main(void) {
    test_stack_offset();
    test_stack_limit();
    test_storage_size_mutation();
    test_stack_size_overflow();
    test_init_alignment();
    test_allocator_alignment();
#if defined(TEST_MCO_WINDOWS_ASM)
    test_stack_size_mutation();
    test_stack_size_layout_overflow();
#endif
    return 0;
}
