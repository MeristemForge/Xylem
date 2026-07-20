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

#include "platform/platform-vmem.h"
#include "assert.h"

#include <stdint.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static void test_reserve_commit_decommit_release(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    size_t   size = page_size * 2;
    uint8_t* ptr  = (uint8_t*)platform_vmem_reserve(size);
    ASSERT(ptr != NULL);
    ASSERT((uintptr_t)ptr % page_size == 0);
    VMEM_ASAN_UNPOISON(ptr, size);
    ASSERT(platform_vmem_commit(ptr, size) == 0);

    ptr[0] = 0x5a;
    ASSERT(ptr[0] == 0x5a);
    ptr[page_size] = 0xa5;
    ASSERT(ptr[page_size] == 0xa5);

    ASSERT(platform_vmem_decommit(ptr + page_size, page_size) == 0);
    VMEM_ASAN_POISON(ptr + page_size, page_size);
    VMEM_ASAN_UNPOISON(ptr + page_size, page_size);
    ASSERT(platform_vmem_commit(ptr + page_size, page_size) == 0);
    ptr[page_size] = 0x3c;
    ASSERT(ptr[page_size] == 0x3c);

    ptr[0] = 0xc3;
    ASSERT(ptr[0] == 0xc3);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}

static void test_guard(void) {
    size_t   page_size = platform_vmem_page_size();
    uint8_t* ptr;
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION info;
#endif

    ASSERT(page_size > 0);
    ptr = (uint8_t*)platform_vmem_reserve(page_size);
    ASSERT(ptr != NULL);
    VMEM_ASAN_UNPOISON(ptr, page_size);
    ASSERT(platform_vmem_commit(ptr, page_size) == 0);
    ASSERT(platform_vmem_guard(ptr, page_size) == 0);
#if defined(_WIN32)
    ASSERT(VirtualQuery(ptr, &info, sizeof(info)) == sizeof(info));
    ASSERT((info.Protect & 0xffU) == PAGE_READWRITE);
    ASSERT((info.Protect & PAGE_GUARD) != 0);
#else
    ptr[0] = 0x5a;
    ASSERT(ptr[0] == 0x5a);
#endif
    ASSERT(platform_vmem_release(ptr, page_size) == 0);
}

#ifdef VMEM_ASAN
static void test_shadow_state_is_caller_owned(void) {
    size_t   page_size = platform_vmem_page_size();
    uint8_t* ptr       = (uint8_t*)platform_vmem_reserve(page_size);

    ASSERT(ptr != NULL);
    VMEM_ASAN_POISON(ptr, page_size);
    ASSERT(__asan_address_is_poisoned(ptr) != 0);
    ASSERT(platform_vmem_commit(ptr, page_size) == 0);
    ASSERT(__asan_address_is_poisoned(ptr) != 0);

    VMEM_ASAN_UNPOISON(ptr, page_size);
    ASSERT(__asan_address_is_poisoned(ptr) == 0);
    ASSERT(platform_vmem_decommit(ptr, page_size) == 0);
    ASSERT(__asan_address_is_poisoned(ptr) == 0);

    VMEM_ASAN_POISON(ptr, page_size);
    ASSERT(__asan_address_is_poisoned(ptr) != 0);
    VMEM_ASAN_UNPOISON(ptr, page_size);
    ASSERT(platform_vmem_release(ptr, page_size) == 0);
}
#endif

#if defined(_WIN32)
static void test_page_size_lookup_is_cached(void) {
    const int       lookup_count = 4096;
    LARGE_INTEGER   start;
    LARGE_INTEGER   end;
    SYSTEM_INFO     info;
    int64_t         lookup_ticks = INT64_MAX;
    int64_t         system_ticks = INT64_MAX;
    volatile size_t sink         = 0;

    ASSERT(platform_vmem_page_size() > 0);
    GetSystemInfo(&info);
    for (int sample = 0; sample < 5; sample++) {
        QueryPerformanceCounter(&start);
        for (int i = 0; i < lookup_count; i++) {
            sink += platform_vmem_page_size();
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
    test_reserve_commit_decommit_release();
    test_guard();
#ifdef VMEM_ASAN
    test_shadow_state_is_caller_owned();
#endif
#if defined(_WIN32)
    test_page_size_lookup_is_cached();
#endif
    return 0;
}
