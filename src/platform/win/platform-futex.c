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

/* WaitOnAddress / WakeByAddress* live in api-ms-win-core-synch-l1-2-0,
 * whose import library is Synchronization.lib (linked in CMakeLists).
 * Available since Windows 8 / Server 2012. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

void platform_futex_wait(_Atomic uint32_t* addr, uint32_t expected) {
    /* WaitOnAddress compares *addr against the value pointed to by its
     * second argument, so pass the address of a local copy. A FALSE
     * return (timeout/spurious) is fine: the caller re-checks in a loop. */
    WaitOnAddress((volatile void*)addr, &expected, sizeof(expected), INFINITE);
}

void platform_futex_wake_one(_Atomic uint32_t* addr) {
    WakeByAddressSingle((PVOID)addr);
}

void platform_futex_wake_all(_Atomic uint32_t* addr) {
    WakeByAddressAll((PVOID)addr);
}
