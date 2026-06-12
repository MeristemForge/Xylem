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
 * Per-thread futex-backed wake semaphore.
 *
 * The wake primitive for OS-thread waiters in the handoff primitives
 * (cond, channel): a thread blocks until a waker hands it a token. It is
 * a genuine counting semaphore -- a post landing before the wait banks a
 * token the next wait consumes -- so it is a drop-in for a kernel wake
 * semaphore and keeps the consumers' claim/arbitrate races valid.
 *
 * The token count is thread-local and lives for the thread's lifetime, so
 * a waker holding only a pointer to it can signal safely even after the
 * waiter's call frame is gone. The blocking backend is platform_futex
 * (WaitOnAddress on Windows, SYS_futex on Linux), markedly cheaper than a
 * kernel semaphore on Windows. Coroutines never use this; they park on
 * the scheduler.
 */

typedef struct tls_wake_s tls_wake_t;

/**
 * @brief Get the calling thread's wake object, creating it on first use.
 *
 * @return The thread's wake object, or NULL on allocation failure (the
 *         caller must then neither block nor enqueue).
 */
extern tls_wake_t* tls_wake_self(void);

/**
 * @brief Acquire a token, blocking until one is available.
 *
 * @param w  Wake object.
 */
extern void tls_wake_wait(tls_wake_t* w);

/**
 * @brief Acquire a token, blocking up to @p timeout_ms milliseconds.
 *
 * @param w           Wake object.
 * @param timeout_ms  Maximum time to block, in milliseconds.
 *
 * @return true if a token was acquired, false if the timeout elapsed.
 */
extern bool tls_wake_timedwait(tls_wake_t* w, uint64_t timeout_ms);

/**
 * @brief Release a token and wake the thread if it is blocked.
 *
 * Safe to call on another thread's wake object through a pointer captured
 * earlier, since the object outlives any single wait.
 *
 * @param w  Wake object to signal.
 */
extern void tls_wake_signal(tls_wake_t* w);
