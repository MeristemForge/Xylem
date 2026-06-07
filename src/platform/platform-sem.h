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

#include <stdint.h>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>

typedef dispatch_semaphore_t platform_sem_t;
#endif

#if defined(__linux__)
#include <semaphore.h>

typedef sem_t platform_sem_t;
#endif

#if defined(_WIN32)
#include <windows.h>

typedef HANDLE platform_sem_t;
#endif

/**
 * @brief Create a semaphore with an initial count.
 *
 * @param value  Initial semaphore count.
 *
 * @return Semaphore handle, or NULL on failure.
 */
extern platform_sem_t* platform_sem_create(unsigned int value);

/**
 * @brief Destroy a semaphore and free its memory.
 *
 * @param sem  Semaphore to destroy.
 */
extern void platform_sem_destroy(platform_sem_t* sem);

/**
 * @brief Increment (signal) the semaphore.
 *
 * @param sem  Semaphore to signal.
 */
extern void platform_sem_post(platform_sem_t* sem);

/**
 * @brief Wait on the semaphore indefinitely.
 *
 * @param sem  Semaphore to wait on.
 */
extern void platform_sem_wait(platform_sem_t* sem);

/**
 * @brief Wait on the semaphore with a timeout.
 *
 * @param sem        Semaphore to wait on.
 * @param timeout_ms Maximum time to wait in milliseconds.
 *
 * @return 0 if the semaphore was acquired, -1 on timeout.
 */
extern int platform_sem_timedwait(platform_sem_t* sem, uint64_t timeout_ms);
