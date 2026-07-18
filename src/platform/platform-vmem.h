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
 * Virtual-memory lifecycle calls bypass ASan's allocation interceptors.
 * Explicit poison tracks decommitted pages, and unpoison makes recommitted
 * pages accessible to the next occupant. Non-ASan builds need no shadow work.
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
#define VMEM_ASAN_POISON(ptr, size) __asan_poison_memory_region((ptr), (size))
#define VMEM_ASAN_UNPOISON(ptr, size) \
    __asan_unpoison_memory_region((ptr), (size))
#else
#define VMEM_ASAN_POISON(ptr, size)   ((void)0)
#define VMEM_ASAN_UNPOISON(ptr, size) ((void)0)
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
 * @brief Reserve a virtual-memory address range.
 *
 * @param size  Number of page-aligned bytes to reserve.
 *
 * @return Page-aligned base address, or NULL on failure.
 */
extern void* platform_vmem_reserve(size_t size);

/**
 * @brief Commit pages for read/write access.
 *
 * Committing an already committed range is permitted.
 *
 * @param ptr   Page-aligned address within a reservation.
 * @param size  Number of page-aligned bytes to commit.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_vmem_commit(void* ptr, size_t size);

/**
 * @brief Decommit pages while preserving their reservation.
 *
 * Decommit forfeits previous contents. The range must not be accessed until
 * it is committed again.
 *
 * @param ptr   Page-aligned address within a reservation.
 * @param size  Number of page-aligned bytes to decommit.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_vmem_decommit(void* ptr, size_t size);

/**
 * @brief Release a complete virtual-memory reservation.
 *
 * @param ptr   Base address returned by platform_vmem_reserve.
 * @param size  Complete reservation size passed to platform_vmem_reserve.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_vmem_release(void* ptr, size_t size);

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
