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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

size_t platform_vmem_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
}

void* platform_vmem_reserve(size_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}

int platform_vmem_commit(void* ptr, size_t size) {
    void* committed = VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE);
    if (committed != ptr) {
        return -1;
    }
    VMEM_ASAN_UNPOISON(ptr, size);
    return 0;
}

int platform_vmem_decommit(void* ptr, size_t size) {
    VMEM_ASAN_POISON(ptr, size);
    return VirtualFree(ptr, size, MEM_DECOMMIT) ? 0 : -1;
}

int platform_vmem_release(void* ptr, size_t size) {
    (void)size;
    return VirtualFree(ptr, 0, MEM_RELEASE) ? 0 : -1;
}

int platform_vmem_protect(void* ptr, size_t size, platform_vmem_prot_t prot) {
    DWORD flags;
    if (prot == PLATFORM_VMEM_PROT_NONE) {
        flags = PAGE_NOACCESS;
    } else if (prot & PLATFORM_VMEM_PROT_WRITE) {
        flags = PAGE_READWRITE;
    } else {
        flags = PAGE_READONLY;
    }
    DWORD old;
    return VirtualProtect(ptr, size, flags, &old) ? 0 : -1;
}
