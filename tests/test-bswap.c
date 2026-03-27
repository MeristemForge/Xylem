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

#include <math.h>
#include <string.h>

static inline uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static inline uint64_t double_bits(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}

/* uint16: 0x1234 -> 0x3412 */
static void test_bswap_uint16_typical(void) {
    ASSERT(xylem_bswap((uint16_t)0x1234U) == 0x3412U);
}

/* int16: -1 (0xFFFF) swaps to itself. */
static void test_bswap_int16_boundary(void) {
    ASSERT(xylem_bswap((int16_t)-1) == -1);
}

/* uint32: 0x12345678 -> 0x78563412 */
static void test_bswap_uint32_typical(void) {
    ASSERT(xylem_bswap((uint32_t)0x12345678U) == 0x78563412U);
}

/* uint64: 0x0123456789ABCDEF -> 0xEFCDAB8967452301 */
static void test_bswap_uint64_typical(void) {
    ASSERT(xylem_bswap((uint64_t)0x0123456789ABCDEFULL) == 0xEFCDAB8967452301ULL);
}

/* float: verify bit-pattern reversal via uint32 comparison. */
static void test_bswap_float_bitpattern(void) {
    float    input = 123.456f;
    uint32_t original_bits = float_bits(input);
    float    swapped = xylem_bswap(input);
    uint32_t swapped_bits = float_bits(swapped);
    ASSERT(swapped_bits == xylem_bswap_u32(original_bits));
}

/* double NaN: verify bit-pattern reversal via uint64 comparison. */
static void test_bswap_double_nan(void) {
    double   input = NAN;
    uint64_t original_bits = double_bits(input);
    double   swapped = xylem_bswap(input);
    uint64_t swapped_bits = double_bits(swapped);
    ASSERT(swapped_bits == xylem_bswap_u64(original_bits));
}

/* Zero swaps to zero for all types. */
static void test_bswap_zero_all_types(void) {
    ASSERT(xylem_bswap((uint16_t)0) == 0);
    ASSERT(xylem_bswap((int16_t)0) == 0);
    ASSERT(xylem_bswap((uint32_t)0U) == 0U);
    ASSERT(xylem_bswap((int32_t)0) == 0);
    ASSERT(xylem_bswap((uint64_t)0ULL) == 0ULL);
    ASSERT(xylem_bswap((int64_t)0) == 0);
    ASSERT(xylem_bswap(0.0f) == 0.0f);
    ASSERT(xylem_bswap(0.0) == 0.0);
}

/* Double swap (swap twice) recovers the original value. */
static void test_bswap_roundtrip(void) {
    ASSERT(xylem_bswap(xylem_bswap((uint16_t)0xABCDU)) == 0xABCDU);
    ASSERT(xylem_bswap(xylem_bswap((uint32_t)0xDEADBEEFU)) == 0xDEADBEEFU);
    ASSERT(xylem_bswap(xylem_bswap((uint64_t)0xCAFEBABEDEADFACEULL)) == 0xCAFEBABEDEADFACEULL);

    float f = 3.14f;
    ASSERT(float_bits(xylem_bswap(xylem_bswap(f))) == float_bits(f));

    double d = 2.71828;
    ASSERT(double_bits(xylem_bswap(xylem_bswap(d))) == double_bits(d));
}

int main(void) {
    test_bswap_uint16_typical();
    test_bswap_int16_boundary();
    test_bswap_uint32_typical();
    test_bswap_uint64_typical();
    test_bswap_float_bitpattern();
    test_bswap_double_nan();
    test_bswap_zero_all_types();
    test_bswap_roundtrip();
    return 0;
}
