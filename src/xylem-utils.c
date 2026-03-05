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

#include "xylem.h"

uint64_t xylem_utils_getnow(xylem_time_precision_t precision) {
    struct timespec tsc;
    (void)timespec_get(&tsc, TIME_UTC);

    switch (precision) {
    case XYLEM_TIME_PRECISION_SEC:
        return tsc.tv_sec;
    case XYLEM_TIME_PRECISION_MSEC:
        return tsc.tv_sec * 1000ULL + tsc.tv_nsec / 1000000ULL;
    case XYLEM_TIME_PRECISION_USEC:
        return tsc.tv_sec * 1000000ULL + tsc.tv_nsec / 1000ULL;
    case XYLEM_TIME_PRECISION_NSEC:
        return tsc.tv_sec * 1000000000ULL + tsc.tv_nsec;
    default:
        return UINT64_MAX;
    }
}

xylem_endian_t xylem_utils_getendian(void) {
    return (*((unsigned char*)(&(unsigned short){0x01}))) ? XYLEM_ENDIAN_LE
                                                          : XYLEM_ENDIAN_BE;
}

int xylem_utils_getprng(int min, int max) {
    static _Atomic unsigned int seed = 0;
    unsigned int s = atomic_load(&seed);
    if (s == 0) {
        s = (unsigned int)time(NULL);
        atomic_store(&seed, s);
        srand(s);
    }
    return min +
           (int)((double)((double)(max) - (min) + 1.0) * (rand() / ((RAND_MAX) + 1.0)));
}
