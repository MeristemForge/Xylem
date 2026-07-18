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

#include <sys/mman.h>
#include <unistd.h>

size_t platform_vmem_page_size(void) {
    return (size_t)sysconf(_SC_PAGESIZE);
}

void* platform_vmem_reserve(size_t size) {
    void* ptr = mmap(
        NULL,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    if (ptr == MAP_FAILED) {
        return NULL;
    }
#if (defined(__linux__) || defined(__ANDROID__)) && defined(MADV_NOHUGEPAGE)
    (void)madvise(ptr, size, MADV_NOHUGEPAGE);
#endif
    return ptr;
}

int platform_vmem_commit(void* ptr, size_t size) {
#if defined(__APPLE__) && defined(MADV_FREE_REUSE)
    if (madvise(ptr, size, MADV_FREE_REUSE) != 0) {
        return -1;
    }
#endif
    VMEM_ASAN_UNPOISON(ptr, size);
    return 0;
}

int platform_vmem_decommit(void* ptr, size_t size) {
    VMEM_ASAN_POISON(ptr, size);
#if defined(__APPLE__) && defined(MADV_FREE_REUSABLE)
    return madvise(ptr, size, MADV_FREE_REUSABLE) == 0 ? 0 : -1;
#else
    return madvise(ptr, size, MADV_DONTNEED) == 0 ? 0 : -1;
#endif
}

int platform_vmem_release(void* ptr, size_t size) {
    return munmap(ptr, size) == 0 ? 0 : -1;
}

int platform_vmem_protect(void* ptr, size_t size, platform_vmem_prot_t prot) {
    int flags = PROT_NONE;
    if (prot & PLATFORM_VMEM_PROT_READ) {
        flags |= PROT_READ;
    }
    if (prot & PLATFORM_VMEM_PROT_WRITE) {
        flags |= PROT_WRITE;
    }
    return mprotect(ptr, size, flags) == 0 ? 0 : -1;
}
