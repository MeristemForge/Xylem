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


/** @brief Virtual memory protection flags. */
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
 * @brief Reserve virtual address space without committing physical memory.
 *
 * The reserved region is inaccessible until committed via platform_vmem_commit.
 *
 * @param size  Number of bytes to reserve (rounded up to page boundary).
 *
 * @return Base address of the reserved region, or NULL on failure.
 */
extern void* platform_vmem_reserve(size_t size);

/**
 * @brief Commit physical pages within a previously reserved region.
 *
 * Committed pages become read/write accessible.
 *
 * @param ptr   Page-aligned address within a reserved region.
 * @param size  Number of bytes to commit (must be page-aligned).
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_vmem_commit(void* ptr, size_t size);

/**
 * @brief Release physical pages without changing protection.
 *
 * The address range remains accessible (mapped RW) but the OS may reclaim
 * the physical backing.  Next access will fault in a fresh zero page.
 *
 * @param ptr   Page-aligned address within a committed region.
 * @param size  Number of bytes to decommit (must be page-aligned).
 */
extern void platform_vmem_decommit(void* ptr, size_t size);

/**
 * @brief Release a reserved region entirely (commit + address space).
 *
 * @param ptr   Base address returned by platform_vmem_reserve.
 * @param size  Size passed to the original reservation.
 */
extern void platform_vmem_release(void* ptr, size_t size);

/**
 * @brief Change memory protection on a committed region.
 *
 * @param ptr   Page-aligned address.
 * @param size  Number of bytes to protect (must be page-aligned).
 * @param prot  Protection flags (combination of platform_vmem_prot_t).
 *
 * @return 0 on success, -1 on failure.
 */
extern int platform_vmem_protect(void* ptr, size_t size, platform_vmem_prot_t prot);
