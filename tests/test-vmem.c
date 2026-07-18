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

static void test_reserve_commit_decommit_release(void) {
    size_t page_size = platform_vmem_page_size();
    ASSERT(page_size > 0);

    size_t   size = page_size * 2;
    uint8_t* ptr  = (uint8_t*)platform_vmem_reserve(size);
    ASSERT(ptr != NULL);
    ASSERT((uintptr_t)ptr % page_size == 0);
    ASSERT(platform_vmem_commit(ptr, size) == 0);

    ptr[0] = 0x5a;
    ASSERT(ptr[0] == 0x5a);
    ptr[page_size] = 0xa5;
    ASSERT(ptr[page_size] == 0xa5);

    ASSERT(platform_vmem_decommit(ptr + page_size, page_size) == 0);
    ASSERT(platform_vmem_commit(ptr + page_size, page_size) == 0);
    ptr[page_size] = 0x3c;
    ASSERT(ptr[page_size] == 0x3c);

    ptr[0] = 0xc3;
    ASSERT(ptr[0] == 0xc3);
    ASSERT(platform_vmem_release(ptr, size) == 0);
}

int main(void) {
    test_reserve_commit_decommit_release();
    return 0;
}
