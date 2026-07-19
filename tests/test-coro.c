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

#if defined(_WIN32) && !defined(MCO_USE_FIBERS) && \
    !defined(MCO_USE_UCONTEXT) && !defined(MCO_USE_ASYNCIFY) && \
    (defined(__x86_64__) || defined(_M_X64))
#define TEST_MCO_WINDOWS_ASM 1
#endif

typedef struct {
    size_t slack_size;
    int    alloc_calls;
} _slack_alloc_ctx_t;

static void _empty_entry(mco_coro* co) {
    (void)co;
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
#elif defined(_WIN32) && defined(MCO_USE_FIBERS)
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
#endif

int main(void) {
    test_stack_offset();
    test_stack_limit();
    test_storage_size_mutation();
    test_stack_size_overflow();
#if defined(TEST_MCO_WINDOWS_ASM)
    test_stack_size_mutation();
#endif
    return 0;
}
