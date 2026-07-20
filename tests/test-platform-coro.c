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
#include "platform/platform.h"

#include <stdint.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static void _assert_invalid(
    void*  slot,
    size_t slot_size,
    size_t stack_offset,
    size_t stack_size) {
    ASSERT(platform_coro_prepare_slot(
               slot,
               slot_size,
               stack_offset,
               stack_size) == -1);
}

#if defined(_WIN32)
static void _assert_page(
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

static void _assert_embedded_layout(uint8_t* ptr, size_t page_size) {
    _assert_page(ptr, MEM_COMMIT, PAGE_READWRITE, 0);
    _assert_page(ptr + page_size, MEM_COMMIT, PAGE_READWRITE, 0);
    _assert_page(ptr + page_size * 2, MEM_RESERVE, 0, 0);
    _assert_page(ptr + page_size * 3, MEM_COMMIT, PAGE_READWRITE, 1);
    _assert_page(ptr + page_size * 4, MEM_COMMIT, PAGE_READWRITE, 0);
}

#endif

static void test_external_stack(void) {
    size_t   page_size = platform_vmem_page_size();
    size_t   size      = page_size * 2;
    uint8_t* ptr       = (uint8_t*)platform_vmem_reserve(size);

    ASSERT(page_size > 0);
    ASSERT(ptr != NULL);
    ASSERT(platform_coro_prepare_slot(ptr, size, 0, 0) == 0);
    ptr[0]        = 0x5a;
    ptr[size - 1] = 0xa5;
    ASSERT(ptr[0] == 0x5a);
    ASSERT(ptr[size - 1] == 0xa5);
    ASSERT(platform_coro_initial_stack_limit(NULL, 0) == NULL);
    ASSERT(platform_vmem_decommit(ptr, size) == 0);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}

static void test_invalid_null_or_zero(void) {
    size_t   page_size = platform_vmem_page_size();
    uint8_t* ptr       = (uint8_t*)platform_vmem_reserve(page_size);

    ASSERT(page_size > 0);
    ASSERT(ptr != NULL);
    _assert_invalid(NULL, page_size, 0, 0);
    _assert_invalid(ptr, 0, 0, 0);
    ASSERT(platform_coro_initial_stack_limit(NULL, page_size) == NULL);
    ASSERT(platform_coro_initial_stack_limit(ptr, 0) == NULL);
    ASSERT(platform_vmem_release(ptr, page_size) == 0);
}

#if defined(_WIN32)
static void test_embedded_stack_layout(void) {
    size_t   page_size = platform_vmem_page_size();
    size_t   size      = page_size * 5;
    uint8_t* ptr       = (uint8_t*)platform_vmem_reserve(size);

    ASSERT(page_size > 0);
    ASSERT(ptr != NULL);
    ASSERT(platform_coro_prepare_slot(
               ptr,
               size,
               page_size * 2,
               page_size * 3) == 0);
    _assert_embedded_layout(ptr, page_size);
    ASSERT(platform_coro_initial_stack_limit(
               ptr + page_size * 2,
               page_size * 3) == ptr + page_size * 4);

    ptr[page_size * 4] = 0x5a;
    ASSERT(ptr[page_size * 4] == 0x5a);
    ASSERT(platform_vmem_decommit(ptr, size) == 0);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}

static void test_invalid_non_page_aligned_ranges(void) {
    size_t   page_size = platform_vmem_page_size();
    size_t   size      = page_size * 5;
    uint8_t* ptr       = (uint8_t*)platform_vmem_reserve(size);

    ASSERT(page_size > 0);
    ASSERT(ptr != NULL);
    _assert_invalid(ptr + 1, size, 0, 0);
    _assert_invalid(ptr, size - 1, 0, 0);
    _assert_invalid(ptr, size, page_size * 2 + 1, page_size * 2);
    _assert_invalid(ptr, size, page_size * 2, page_size * 2 - 1);
    ASSERT(platform_coro_initial_stack_limit(
               ptr + page_size * 2 + 1,
               page_size * 2) == NULL);
    ASSERT(platform_coro_initial_stack_limit(
               ptr + page_size * 2,
               page_size * 2 - 1) == NULL);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}

static void test_invalid_half_external_stack(void) {
    size_t   page_size = platform_vmem_page_size();
    size_t   size      = page_size * 5;
    uint8_t* ptr       = (uint8_t*)platform_vmem_reserve(size);

    ASSERT(page_size > 0);
    ASSERT(ptr != NULL);
    _assert_invalid(ptr, size, page_size * 2, 0);
    _assert_invalid(ptr, size, 0, page_size * 2);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}

static void test_invalid_layout_preserves_slot(void) {
    size_t   page_size  = platform_vmem_page_size();
    size_t   page_count = 5;
    size_t   size       = page_size * page_count;
    uint8_t* ptr        = (uint8_t*)platform_vmem_reserve(size);

    ASSERT(page_size > 0);
    ASSERT(ptr != NULL);
    ASSERT(platform_vmem_commit(ptr, size) == 0);
    ptr[0]             = 0x5a;
    ptr[page_size * 2] = 0xa5;
    ptr[size - 1]      = 0x3c;
    ASSERT(platform_coro_prepare_slot(ptr, size, page_size * 2, 0) == -1);
    for (size_t i = 0; i < page_count; i++) {
        _assert_page(ptr + i * page_size, MEM_COMMIT, PAGE_READWRITE, 0);
    }
    ASSERT(ptr[0] == 0x5a);
    ASSERT(ptr[page_size * 2] == 0xa5);
    ASSERT(ptr[size - 1] == 0x3c);
    ASSERT(platform_vmem_decommit(ptr, size) == 0);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}

static void test_invalid_stack_outside_slot(void) {
    size_t   page_size = platform_vmem_page_size();
    size_t   size      = page_size * 5;
    uint8_t* ptr       = (uint8_t*)platform_vmem_reserve(size);

    ASSERT(page_size > 0);
    ASSERT(ptr != NULL);
    _assert_invalid(ptr, size, size, page_size * 2);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}

static void test_invalid_missing_metadata(void) {
    size_t   page_size = platform_vmem_page_size();
    size_t   size      = page_size * 5;
    uint8_t* ptr       = (uint8_t*)platform_vmem_reserve(size);

    ASSERT(page_size > 0);
    ASSERT(ptr != NULL);
    _assert_invalid(ptr, size, 0, page_size * 2);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}

static void test_invalid_stack_too_small(void) {
    size_t   page_size = platform_vmem_page_size();
    size_t   size      = page_size * 5;
    uint8_t* ptr       = (uint8_t*)platform_vmem_reserve(size);

    ASSERT(page_size > 0);
    ASSERT(ptr != NULL);
    _assert_invalid(ptr, size, page_size * 2, page_size);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}
#endif

int main(void) {
    test_external_stack();
    test_invalid_null_or_zero();
#if defined(_WIN32)
    test_embedded_stack_layout();
    test_invalid_non_page_aligned_ranges();
    test_invalid_half_external_stack();
    test_invalid_layout_preserves_slot();
    test_invalid_stack_outside_slot();
    test_invalid_missing_metadata();
    test_invalid_stack_too_small();
#endif
    return 0;
}
