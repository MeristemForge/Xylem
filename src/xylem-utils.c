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

#include "xylem/xylem-utils.h"

#include "platform/platform-info.h"

#include <stdatomic.h>
#include <time.h>

uint64_t xylem_utils_getnow(xylem_time_precision_t precision) {
    struct timespec tsc;
    (void)timespec_get(&tsc, TIME_UTC);

    switch (precision) {
    case XYLEM_TIME_PRECISION_SEC:
        return (uint64_t)tsc.tv_sec;
    case XYLEM_TIME_PRECISION_MSEC:
        return (uint64_t)tsc.tv_sec * 1000ULL + (uint64_t)tsc.tv_nsec / 1000000ULL;
    case XYLEM_TIME_PRECISION_USEC:
        return (uint64_t)tsc.tv_sec * 1000000ULL + (uint64_t)tsc.tv_nsec / 1000ULL;
    case XYLEM_TIME_PRECISION_NSEC:
        return (uint64_t)tsc.tv_sec * 1000000000ULL + (uint64_t)tsc.tv_nsec;
    default:
        return UINT64_MAX;
    }
}

xylem_endian_t xylem_utils_getendian(void) {
    return (*((unsigned char*)(&(unsigned short){0x01}))) ? XYLEM_ENDIAN_LE
                                                          : XYLEM_ENDIAN_BE;
}

/**
 * splitmix64 — bijective hash used to diffuse a monotonic counter
 * into a full-period, non-repeating sequence over 2^64.
 */
static uint64_t _utils_splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

int xylem_utils_getprng(int min, int max) {
    static _Atomic uint64_t seed    = 0;
    static _Atomic uint64_t counter = 0;

    /* One-time seed from OS entropy. */
    if (atomic_load(&seed) == 0) {
        uint64_t s;
        if (!platform_info_getrandom(&s, sizeof(s)) || s == 0) {
            s = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&seed;
            s |= 1;
        }
        uint64_t expected = 0;
        atomic_compare_exchange_strong(&seed, &expected, s);
    }

    /* Each call gets a unique counter value. */
    uint64_t seq = atomic_fetch_add(&counter, 1);
    uint64_t r   = _utils_splitmix64(seq + atomic_load(&seed));

    return min + (int)(r % (uint64_t)(max - min + 1));
}
