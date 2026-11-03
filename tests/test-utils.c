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
#include "assert.h"

static void test_time_precision_units(void) {
    const xylem_time_precision_t precisions[] = {
        XYLEM_TIME_PRECISION_MSEC,
        XYLEM_TIME_PRECISION_USEC,
        XYLEM_TIME_PRECISION_NSEC,
    };
    const uint64_t units[] = {1000, 1000000, 1000000000};
    for (size_t i = 0; i < sizeof(precisions) / sizeof(precisions[0]); i++) {
        uint64_t before = xylem_utils_getnow(XYLEM_TIME_PRECISION_SEC);
        uint64_t value  = xylem_utils_getnow(precisions[i]);
        uint64_t after  = xylem_utils_getnow(XYLEM_TIME_PRECISION_SEC);
        uint64_t lower  = before < after ? before : after;
        uint64_t upper  = before > after ? before : after;
        ASSERT(before > 0 && after > 0);
        ASSERT(value / units[i] >= lower);
        ASSERT(value / units[i] <= upper);
    }
}

static void test_getprng_range(void) {
    for (int i = 0; i < 1000; i++) {
        int v = xylem_utils_getprng(10, 20);
        ASSERT(v >= 10);
        ASSERT(v <= 20);
    }
}

static void test_getprng_single(void) {
    ASSERT(xylem_utils_getprng(42, 42) == 42);
}

int main(void) {
    test_time_precision_units();
    test_getprng_range();
    test_getprng_single();
    return 0;
}
