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

#include <string.h>

/* RFC 4231 Test Case 1 */
static void test_rfc4231_case1(void) {
    uint8_t key[20];
    memset(key, 0x0b, 20);

    const uint8_t msg[] = "Hi There";
    uint8_t out[32];

    xylem_hmac256_compute(key, 20, msg, 8, out);

    uint8_t expected[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };
    ASSERT(memcmp(out, expected, 32) == 0);
}

/* RFC 4231 Test Case 2: "Jefe" / "what do ya want for nothing?" */
static void test_rfc4231_case2(void) {
    const uint8_t key[] = "Jefe";
    const uint8_t msg[] = "what do ya want for nothing?";
    uint8_t out[32];

    xylem_hmac256_compute(key, 4, msg, 28, out);

    uint8_t expected[32] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
        0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
    };
    ASSERT(memcmp(out, expected, 32) == 0);
}

/* RFC 4231 Test Case 3: key=0xaa*20, msg=0xdd*50 */
static void test_rfc4231_case3(void) {
    uint8_t key[20];
    memset(key, 0xaa, 20);

    uint8_t msg[50];
    memset(msg, 0xdd, 50);

    uint8_t out[32];
    xylem_hmac256_compute(key, 20, msg, 50, out);

    uint8_t expected[32] = {
        0x77, 0x3e, 0xa9, 0x1e, 0x36, 0x80, 0x0e, 0x46,
        0x85, 0x4d, 0xb8, 0xeb, 0xd0, 0x91, 0x81, 0xa7,
        0x29, 0x59, 0x09, 0x8b, 0x3e, 0xf8, 0xc1, 0x22,
        0xd9, 0x63, 0x55, 0x14, 0xce, 0xd5, 0x65, 0xfe,
    };
    ASSERT(memcmp(out, expected, 32) == 0);
}

/* RFC 4231 Test Case 6: key longer than block size (131 bytes) */
static void test_long_key(void) {
    uint8_t key[131];
    memset(key, 0xaa, 131);

    const uint8_t msg[] = "Test Using Larger Than Block-Size Key - Hash Key First";
    uint8_t out[32];

    xylem_hmac256_compute(key, 131, msg, 54, out);

    uint8_t expected[32] = {
        0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f,
        0x0d, 0x8a, 0x26, 0xaa, 0xcb, 0xf5, 0xb7, 0x7f,
        0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28, 0xc5, 0x14,
        0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3, 0x7f, 0x54,
    };
    ASSERT(memcmp(out, expected, 32) == 0);
}

/* Empty message */
static void test_empty_message(void) {
    const uint8_t key[] = "secret";
    uint8_t out[32];

    xylem_hmac256_compute(key, 6, NULL, 0, out);

    /* Just verify it doesn't crash and produces 32 bytes. */
    uint8_t zero[32] = {0};
    ASSERT(memcmp(out, zero, 32) != 0);
}

int main(void) {
    test_rfc4231_case1();
    test_rfc4231_case2();
    test_rfc4231_case3();
    test_long_key();
    test_empty_message();
    return 0;
}
