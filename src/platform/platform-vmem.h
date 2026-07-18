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
 * ASan does not track platform decommit and recommit transitions. Explicit
 * poison marks decommitted pages inaccessible, and unpoison makes recommitted
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
 *
 * @note The range must be committed before it is accessed.
 */
extern void* platform_vmem_reserve(size_t size);

/**
 * @brief Make a reserved range available for read/write access.
 *
 * Committing an already committed range is permitted. The operation may be a
 * platform-specific accounting transition rather than a mapping change.
 *
 * @param ptr   Page-aligned address within a reservation.
 * @param size  Number of page-aligned bytes to commit.
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_vmem_commit(void* ptr, size_t size);

/**
 * @brief Make a range reusable while preserving its reservation.
 *
 * Previous contents become unspecified. The range must not be accessed until
 * it is committed again successfully.
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
 *
 * @note Partial reservation release is not supported.
 */
extern int platform_vmem_release(void* ptr, size_t size);
