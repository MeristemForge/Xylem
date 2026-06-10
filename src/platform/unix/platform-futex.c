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

#include "platform/platform-futex.h"

#if defined(__linux__)

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

void platform_futex_wait(_Atomic uint32_t* addr, uint32_t expected) {
    /* PRIVATE: this word is never shared across processes, which lets the
     * kernel skip the cross-process futex hashing. A return (including
     * EAGAIN when the value already moved, or EINTR) just falls through to
     * the caller's predicate re-check. */
    syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
}

void platform_futex_wake_one(_Atomic uint32_t* addr) {
    syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

void platform_futex_wake_all(_Atomic uint32_t* addr) {
    syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAKE_PRIVATE, INT32_MAX, NULL, NULL, 0);
}

#endif

#if defined(__APPLE__)

#include <os/os_sync_wait_on_address.h>

/* os_sync_wait_on_address is the public address-wait API, available since
 * macOS 14.4 / iOS 17.4. Compares the low `size` bytes of *addr against
 * `value` and sleeps only while they match. */

void platform_futex_wait(_Atomic uint32_t* addr, uint32_t expected) {
    os_sync_wait_on_address(
        (void*)addr,
        (uint64_t)expected,
        sizeof(uint32_t),
        OS_SYNC_WAIT_ON_ADDRESS_NONE
    );
}

void platform_futex_wake_one(_Atomic uint32_t* addr) {
    os_sync_wake_by_address_any(
        (void*)addr,
        sizeof(uint32_t),
        OS_SYNC_WAKE_BY_ADDRESS_NONE
    );
}

void platform_futex_wake_all(_Atomic uint32_t* addr) {
    os_sync_wake_by_address_all(
        (void*)addr,
        sizeof(uint32_t),
        OS_SYNC_WAKE_BY_ADDRESS_NONE
    );
}

#endif
