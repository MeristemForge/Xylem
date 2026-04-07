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
#include <stddef.h>
#include <time.h>

#if defined(__linux__)
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
typedef pid_t platform_tid_t;
typedef pid_t platform_pid_t;
#elif defined(__APPLE__)
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
typedef uint64_t platform_tid_t;
typedef pid_t    platform_pid_t;
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
typedef DWORD platform_tid_t;
typedef DWORD platform_pid_t;
#endif

/**
 * @brief Get the current thread ID.
 *
 * @return Platform-specific thread identifier.
 */
extern platform_tid_t platform_info_gettid(void);

/**
 * @brief Get the current process ID.
 *
 * @return Platform-specific process identifier.
 */
extern platform_pid_t platform_info_getpid(void);

/**
 * @brief Get the number of logical CPU cores.
 *
 * @return Number of available logical processors.
 */
extern int platform_info_getcpus(void);

/**
 * @brief Convert a time_t value to local time.
 *
 * @param time  Pointer to the calendar time to convert.
 * @param tm    Pointer to the struct tm to receive the result.
 */
extern void platform_info_getlocaltime(const time_t* restrict time, struct tm* restrict tm);

/**
 * @brief Convert a time_t value to UTC broken-down time.
 *
 * Uses gmtime_s on Windows, gmtime_r on Unix.
 *
 * @param t   Pointer to the calendar time.
 * @param tm  Pointer to the struct tm to receive the result.
 */
extern void platform_info_gmtime(const time_t* t, struct tm* tm);

/**
 * @brief Convert UTC broken-down time to time_t.
 *
 * Uses _mkgmtime on Windows, timegm on Unix.
 *
 * @param tm  Pointer to the UTC broken-down time.
 *
 * @return Corresponding time_t value, or (time_t)-1 on failure.
 */
extern time_t platform_info_mkgmtime(struct tm* tm);

/**
 * @brief Fill a buffer with cryptographically secure random bytes.
 *
 * Uses BCryptGenRandom on Windows, /dev/urandom on Unix.
 *
 * @param buf  Buffer to fill.
 * @param len  Number of bytes to generate.
 *
 * @return true on success, false on failure.
 */
extern bool platform_info_getrandom(void* buf, size_t len);
