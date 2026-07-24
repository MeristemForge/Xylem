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

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

void platform_futex_wait(_Atomic uint32_t* addr, uint32_t expected) {
    /**
     * PRIVATE avoids cross-process futex hashing; this word is never shared.
     * Any return (EAGAIN/EINTR) just falls through to the caller's re-check.
     */
    syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
}

bool platform_futex_timedwait(
    _Atomic uint32_t* addr, uint32_t expected, uint64_t timeout_ms) {
    /* FUTEX_WAIT takes a *relative* timeout; ETIMEDOUT means the deadline hit. */
    struct timespec ts;
    ts.tv_sec  = (time_t)(timeout_ms / 1000);
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    long r = syscall(
        SYS_futex, (uint32_t*)addr, FUTEX_WAIT_PRIVATE, expected, &ts, NULL, 0);
    if (r == -1 && errno == ETIMEDOUT) {
        return false;
    }
    return true;
}

void platform_futex_signal(_Atomic uint32_t* addr) {
    syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

void platform_futex_broadcast(_Atomic uint32_t* addr) {
    syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAKE_PRIVATE, INT32_MAX, NULL, NULL, 0);
}

#endif

#if defined(__APPLE__)

#include <errno.h>

#if __has_include(<os/os_sync_wait_on_address.h>)
#include <os/os_sync_wait_on_address.h>

/* os_sync_wait_on_address is the public address-wait API (macOS 14.4+). */

void platform_futex_wait(_Atomic uint32_t* addr, uint32_t expected) {
    os_sync_wait_on_address(
        (void*)addr,
        (uint64_t)expected,
        sizeof(uint32_t),
        OS_SYNC_WAIT_ON_ADDRESS_NONE
    );
}

bool platform_futex_timedwait(
    _Atomic uint32_t* addr, uint32_t expected, uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return atomic_load(addr) != expected;
    }
    /* Relative timeout in ns against the mach absolute clock; ETIMEDOUT = deadline. */
    int r = os_sync_wait_on_address_with_timeout(
        (void*)addr,
        (uint64_t)expected,
        sizeof(uint32_t),
        OS_SYNC_WAIT_ON_ADDRESS_NONE,
        OS_CLOCK_MACH_ABSOLUTE_TIME,
        timeout_ms * 1000000ULL
    );
    return !(r == -1 && errno == ETIMEDOUT);
}

void platform_futex_signal(_Atomic uint32_t* addr) {
    os_sync_wake_by_address_any(
        (void*)addr,
        sizeof(uint32_t),
        OS_SYNC_WAKE_BY_ADDRESS_NONE
    );
}

void platform_futex_broadcast(_Atomic uint32_t* addr) {
    os_sync_wake_by_address_all(
        (void*)addr,
        sizeof(uint32_t),
        OS_SYNC_WAKE_BY_ADDRESS_NONE
    );
}

#else

/* Private SPI fallback for SDKs predating the public address-wait API.
   <sys/ulock.h> is not shipped in the public SDK — declare what we need. */

#define UL_COMPARE_AND_WAIT  0x01
#define ULF_WAKE_ALL         0x00000100

extern int __ulock_wait2(
    uint32_t operation,
    void*    addr,
    uint64_t value,
    uint64_t timeout_ns,
    uint64_t value2);
extern int __ulock_wake(
    uint32_t operation, void* addr, uint64_t wake_value);

void platform_futex_wait(_Atomic uint32_t* addr, uint32_t expected) {
    __ulock_wait2(
        UL_COMPARE_AND_WAIT,
        (void*)addr,
        (uint64_t)expected,
        0,
        0
    );
}

bool platform_futex_timedwait(
    _Atomic uint32_t* addr, uint32_t expected, uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return atomic_load(addr) != expected;
    }
    uint64_t timeout_ns = timeout_ms > UINT64_MAX / 1000000ULL
        ? UINT64_MAX
        : timeout_ms * 1000000ULL;
    int r = __ulock_wait2(
        UL_COMPARE_AND_WAIT,
        (void*)addr,
        (uint64_t)expected,
        timeout_ns,
        0
    );
    return !(r == -1 && errno == ETIMEDOUT);
}

void platform_futex_signal(_Atomic uint32_t* addr) {
    __ulock_wake(UL_COMPARE_AND_WAIT, (void*)addr, 0);
}

void platform_futex_broadcast(_Atomic uint32_t* addr) {
    __ulock_wake(UL_COMPARE_AND_WAIT | ULF_WAKE_ALL, (void*)addr, 0);
}

#endif

#endif
