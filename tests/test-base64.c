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

static void _check_std(const uint8_t* in, size_t n, const char* expected) {
    uint8_t enc[64];
    int elen = xylem_base64_encode_std(in, n, enc, sizeof(enc));
    ASSERT(elen == (int)strlen(expected));
    ASSERT(memcmp(enc, expected, (size_t)elen) == 0);

    uint8_t dec[64];
    int dlen = xylem_base64_decode_std(enc, elen, dec, sizeof(dec));
    ASSERT(dlen == (int)n);
    ASSERT(memcmp(dec, in, n) == 0);
}

static void _check_url(const uint8_t* in, size_t n, const char* expected,
                       bool pad) {
    uint8_t enc[64];
    int elen = xylem_base64_encode_url(in, n, enc, sizeof(enc), pad);
    ASSERT(elen == (int)strlen(expected));
    ASSERT(memcmp(enc, expected, (size_t)elen) == 0);

    uint8_t dec[64];
    int dlen = xylem_base64_decode_url(enc, elen, dec, sizeof(dec), pad);
    ASSERT(dlen == (int)n);
    ASSERT(memcmp(dec, in, n) == 0);
}

static void test_empty_input(void) {
    uint8_t enc[10], dec[10];

    ASSERT(xylem_base64_encode_std(NULL, 0, enc, sizeof(enc)) == 0);
    ASSERT(xylem_base64_decode_std(NULL, 0, dec, sizeof(dec)) == 0);
    ASSERT(xylem_base64_encode_url(NULL, 0, enc, sizeof(enc), true) == 0);
    ASSERT(xylem_base64_encode_url(NULL, 0, enc, sizeof(enc), false) == 0);
    ASSERT(xylem_base64_decode_url(NULL, 0, dec, sizeof(dec), true) == 0);
    ASSERT(xylem_base64_decode_url(NULL, 0, dec, sizeof(dec), false) == 0);
}

static void test_one_byte(void) {
    uint8_t input[] = {0x41};
    _check_std(input, 1, "QQ==");
    _check_url(input, 1, "QQ==", true);
    _check_url(input, 1, "QQ", false);
}

static void test_two_bytes(void) {
    uint8_t input[] = {0x41, 0x42};
    _check_std(input, 2, "QUI=");
    _check_url(input, 2, "QUI", false);
}

static void test_three_bytes(void) {
    uint8_t input[] = {0x41, 0x42, 0x43};
    _check_std(input, 3, "QUJD");
    _check_url(input, 3, "QUJD", false);
}

static void test_multi_block(void) {
    uint8_t input[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x21};
    _check_std(input, 6, "SGVsbG8h");
}

static void test_illegal_characters(void) {
    uint8_t dec[10];

    ASSERT(xylem_base64_decode_std((uint8_t*)"AB!D", 4, dec, sizeof(dec)) == -1);
    ASSERT(xylem_base64_decode_url((uint8_t*)"AB+D", 4, dec, sizeof(dec),
                                   false) == -1);
    ASSERT(xylem_base64_decode_url((uint8_t*)"AB_D", 4, dec, sizeof(dec),
                                   false) >= 0);
}

static void test_malformed_padding(void) {
    uint8_t dec[10];

    ASSERT(xylem_base64_decode_std((uint8_t*)"Q=Q=", 4, dec, sizeof(dec)) == -1);
    ASSERT(xylem_base64_decode_std((uint8_t*)"=QQQ", 4, dec, sizeof(dec)) == -1);
    ASSERT(xylem_base64_decode_std((uint8_t*)"Q===", 4, dec, sizeof(dec)) == -1);
}

static void test_non_multiple_of_4(void) {
    uint8_t dec[10];

    ASSERT(xylem_base64_decode_std((uint8_t*)"QQ", 2, dec, sizeof(dec)) == -1);
    ASSERT(xylem_base64_decode_std((uint8_t*)"QUI", 3, dec, sizeof(dec)) == -1);
    ASSERT(xylem_base64_decode_url((uint8_t*)"QQ", 2, dec, sizeof(dec),
                                   false) >= 0);
    ASSERT(xylem_base64_decode_url((uint8_t*)"QUI", 3, dec, sizeof(dec),
                                   false) >= 0);
    ASSERT(xylem_base64_decode_url((uint8_t*)"QQ", 2, dec, sizeof(dec),
                                   true) == -1);
}

static void test_insufficient_buffer(void) {
    uint8_t input[] = {0x41};
    uint8_t tiny_enc[3];
    uint8_t tiny_dec[2];

    ASSERT(xylem_base64_encode_std(input, 1, tiny_enc, 3) == -1);
    ASSERT(xylem_base64_decode_std((uint8_t*)"QQ==", 4, tiny_dec, 2) == -1);
}

static void test_round_trip(void) {
    uint8_t input[11], enc[32], dec[16];

    for (int n = 0; n <= 10; n++) {
        for (int i = 0; i < n; i++) {
            input[i] = (uint8_t)i;
        }

        int elen = xylem_base64_encode_std(input, n, enc, sizeof(enc));
        ASSERT(elen >= 0);
        int dlen = xylem_base64_decode_std(enc, elen, dec, sizeof(dec));
        ASSERT(dlen == n);
        ASSERT(memcmp(input, dec, n) == 0);

        elen = xylem_base64_encode_url(input, n, enc, sizeof(enc), false);
        ASSERT(elen >= 0);
        dlen = xylem_base64_decode_url(enc, elen, dec, sizeof(dec), false);
        ASSERT(dlen == n);
        ASSERT(memcmp(input, dec, n) == 0);
    }
}

static void test_extreme_bytes(void) {
    uint8_t input[] = {0x00, 0xFF, 0x80, 0x7F};
    uint8_t enc[32], dec[16];

    int elen = xylem_base64_encode_std(input, 4, enc, sizeof(enc));
    ASSERT(elen == 8);
    int dlen = xylem_base64_decode_std(enc, elen, dec, sizeof(dec));
    ASSERT(dlen == 4);
    ASSERT(memcmp(input, dec, 4) == 0);
}

static void test_reject_nonzero_padding_bits(void) {
    uint8_t buf[4];

    int len = xylem_base64_decode_std((const uint8_t*)"QQ==", 4, buf,
                                      sizeof(buf));
    ASSERT(len == 1);
    ASSERT(buf[0] == 0x41);

    len = xylem_base64_decode_std((const uint8_t*)"QR==", 4, buf, sizeof(buf));
    ASSERT(len == -1);
}

int main(void) {
    test_empty_input();
    test_one_byte();
    test_two_bytes();
    test_three_bytes();
    test_multi_block();
    test_illegal_characters();
    test_malformed_padding();
    test_non_multiple_of_4();
    test_insufficient_buffer();
    test_round_trip();
    test_extreme_bytes();
    test_reject_nonzero_padding_bits();
    return 0;
}
