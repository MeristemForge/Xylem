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

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Callback invoked on memory access fault.
 *
 * @param fault_addr  The faulting virtual address.
 *
 * @return true if the fault was handled, false to propagate.
 */
typedef bool (*platform_stackguard_cb_t)(uintptr_t fault_addr);

/**
 * @brief Install a process-wide fault handler for stack growth.
 *
 * @param cb  Callback invoked on access violation / SIGSEGV.
 */
extern void platform_stackguard_install(platform_stackguard_cb_t cb);

/**
 * @brief Remove the fault handler installed by platform_stackguard_install.
 */
extern void platform_stackguard_uninstall(void);

/**
 * @brief Per-thread setup for fault-driven stack growth.
 *
 * On Unix, allocates and registers an alternate signal stack so that
 * SIGSEGV can be delivered when RSP is in uncommitted memory.
 * No-op on Windows (VEH does not use the user stack).
 */
extern void platform_stackguard_thread_init(void);

/**
 * @brief Tear down per-thread state allocated by thread_init.
 */
extern void platform_stackguard_thread_deinit(void);
