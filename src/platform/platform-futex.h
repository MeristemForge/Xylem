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

#include <stdatomic.h>
#include <stdint.h>

/**
 * Address-based wait/wake (a "futex").
 *
 * Lets a thread sleep on a 32-bit word and be woken when another thread
 * changes it -- the kernel keeps no per-object handle, so an uncontended
 * wake costs nothing and the word itself is the wait queue key. Backends:
 * Linux futex(2), Windows WaitOnAddress, macOS os_sync_wait_on_address.
 *
 * The word is caller-owned (typically a lock state embedded in another
 * struct); this module adds no type or allocation. Only OS threads may
 * use it -- a coroutine must never block a worker on a futex.
 *
 * These are raw building blocks with no fairness or ordering guarantee:
 * spurious wakeups are possible, so every wait MUST sit in a loop that
 * re-checks the predicate on the word.
 */

/**
 * @brief Block until the word is woken, if it still holds @p expected.
 *
 * Atomically checks whether *@p addr equals @p expected and, only if so,
 * sleeps until a wake on @p addr (or a spurious wakeup). If the value
 * already differs, returns immediately without blocking -- this closes
 * the race where a waker changes the word and wakes between the caller's
 * own check and this call.
 *
 * @param addr      Word to wait on.
 * @param expected  Value the caller last observed; wait is armed only
 *                  while *addr still equals it.
 *
 * @note May return spuriously. Always call inside a loop that re-tests
 *       the predicate on *addr.
 */
extern void platform_futex_wait(_Atomic uint32_t* addr, uint32_t expected);

/**
 * @brief Wake at most one thread waiting on @p addr.
 *
 * The store that changes *@p addr must happen before this call so a
 * waiter cannot sleep on the stale value after being skipped. No-op,
 * with no syscall on most backends, when no thread is waiting.
 *
 * @param addr  Word other threads may be waiting on.
 */
extern void platform_futex_wake_one(_Atomic uint32_t* addr);

/**
 * @brief Wake all threads waiting on @p addr.
 *
 * Same store-before-wake rule as platform_futex_wake_one. Used when more
 * than one waiter can make progress (e.g. a broadcast).
 *
 * @param addr  Word other threads may be waiting on.
 */
extern void platform_futex_wake_all(_Atomic uint32_t* addr);
