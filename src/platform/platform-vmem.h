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

_Pragma("once")

#include <stddef.h>

/**
 * The pooled allocator recycles virtual addresses without going through
 * ASan's interceptors, so a freshly committed region may still carry stale
 * shadow poison from its previous occupant, causing false errors on the
 * next user's in-bounds writes. VMEM_ASAN_RESET re-validates the region;
 * it compiles to nothing in non-ASan builds.
 */
#if defined(__SANITIZE_ADDRESS__)
#define VMEM_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VMEM_ASAN 1
#endif
#endif

#ifdef VMEM_ASAN
#include <sanitizer/asan_interface.h>
#define VMEM_ASAN_RESET(ptr, size) __asan_unpoison_memory_region((ptr), (size))
#else
#define VMEM_ASAN_RESET(ptr, size) ((void)0)
#endif

/* Virtual memory protection flags. */
typedef enum platform_vmem_prot_e {
    PLATFORM_VMEM_PROT_NONE  = 0,
    PLATFORM_VMEM_PROT_READ  = 1 << 0,
    PLATFORM_VMEM_PROT_WRITE = 1 << 1,
} platform_vmem_prot_t;

/**
 * @brief Return the system page size in bytes.
 *
 * @return Page size in bytes.
 */
extern size_t platform_vmem_page_size(void);

/**
 * @brief Allocate a read/write virtual-memory region.
 *
 * @param size  Number of bytes to allocate (rounded up to page boundary).
 *
 * @return Base address of the allocated region, or NULL on failure.
 */
extern void* platform_vmem_alloc(size_t size);

/**
 * @brief Discard contents while preserving the mapped region.
 *
 * Previous contents become unspecified, but the region remains accessible
 * with its current protection. This operation does not guarantee immediate
 * zeroing or release of system commit charge.
 *
 * @param ptr   Page-aligned address within an allocated region.
 * @param size  Number of bytes to reset (must be page-aligned).
 */
extern void platform_vmem_reset(void* ptr, size_t size);

/**
 * @brief Deallocate an entire virtual-memory region.
 *
 * @param ptr   Base address returned by platform_vmem_alloc.
 * @param size  Size passed to platform_vmem_alloc.
 */
extern void platform_vmem_dealloc(void* ptr, size_t size);

/**
 * @brief Change memory protection on an allocated region.
 *
 * @param ptr   Page-aligned address.
 * @param size  Number of bytes to protect (must be page-aligned).
 * @param prot  Protection flags (combination of platform_vmem_prot_t).
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_vmem_protect(
    void* ptr,
    size_t size,
    platform_vmem_prot_t prot);
