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

static void test_getnow_sec(void) {
    uint64_t t1 = xylem_utils_getnow(XYLEM_TIME_PRECISION_SEC);
    ASSERT(t1 > 0);
    uint64_t t2 = xylem_utils_getnow(XYLEM_TIME_PRECISION_SEC);
    ASSERT(t2 >= t1);
}

static void test_getnow_msec(void) {
    uint64_t t1 = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    ASSERT(t1 > 0);
    uint64_t t2 = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    ASSERT(t2 >= t1);
    uint64_t sec = xylem_utils_getnow(XYLEM_TIME_PRECISION_SEC);
    ASSERT(t2 / 1000 >= sec - 1);
    ASSERT(t2 / 1000 <= sec + 1);
}

static void test_getnow_usec(void) {
    uint64_t t1 = xylem_utils_getnow(XYLEM_TIME_PRECISION_USEC);
    ASSERT(t1 > 0);
    uint64_t t2 = xylem_utils_getnow(XYLEM_TIME_PRECISION_USEC);
    ASSERT(t2 >= t1);
}

static void test_getnow_nsec(void) {
    uint64_t t1 = xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
    ASSERT(t1 > 0);
    uint64_t t2 = xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
    ASSERT(t2 >= t1);
}

static void test_getprng_range(void) {
    for (int i = 0; i < 1000; i++) {
        int v = xylem_utils_getprng(10, 20);
        ASSERT(v >= 10);
        ASSERT(v <= 20);
    }
}

static void test_getprng_single(void) {
    for (int i = 0; i < 100; i++) {
        int v = xylem_utils_getprng(42, 42);
        ASSERT(v == 42);
    }
}

int main(void) {
    test_getnow_sec();
    test_getnow_msec();
    test_getnow_usec();
    test_getnow_nsec();
    test_getprng_range();
    test_getprng_single();
    return 0;
}
