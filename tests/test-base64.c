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

/* Empty input: both standard and URL-safe variants. */
static void test_empty_input(void) {
    uint8_t enc[10], dec[10];

    int elen = xylem_base64_encode_std(NULL, 0, enc, sizeof(enc));
    ASSERT(elen == 0);
    int dlen = xylem_base64_decode_std(NULL, 0, dec, sizeof(dec));
    ASSERT(dlen == 0);

    elen = xylem_base64_encode_url(NULL, 0, enc, sizeof(enc), true);
    ASSERT(elen == 0);
    elen = xylem_base64_encode_url(NULL, 0, enc, sizeof(enc), false);
    ASSERT(elen == 0);

    dlen = xylem_base64_decode_url(NULL, 0, dec, sizeof(dec), true);
    ASSERT(dlen == 0);
    dlen = xylem_base64_decode_url(NULL, 0, dec, sizeof(dec), false);
    ASSERT(dlen == 0);
}

/* 1-byte input: "A" (0x41) -> "QQ==" (std) or "QQ" (url, no pad). */
static void test_one_byte(void) {
    uint8_t input[] = {0x41};
    uint8_t enc[10], dec[10];

    int elen = xylem_base64_encode_std(input, 1, enc, sizeof(enc));
    ASSERT(elen == 4);
    ASSERT(memcmp(enc, "QQ==", 4) == 0);

    int dlen = xylem_base64_decode_std(enc, 4, dec, sizeof(dec));
    ASSERT(dlen == 1);
    ASSERT(dec[0] == 0x41);

    elen = xylem_base64_encode_url(input, 1, enc, sizeof(enc), true);
    ASSERT(elen == 4);
    ASSERT(memcmp(enc, "QQ==", 4) == 0);
    dlen = xylem_base64_decode_url(enc, 4, dec, sizeof(dec), true);
    ASSERT(dlen == 1 && dec[0] == 0x41);

    elen = xylem_base64_encode_url(input, 1, enc, sizeof(enc), false);
    ASSERT(elen == 2);
    ASSERT(memcmp(enc, "QQ", 2) == 0);
    dlen = xylem_base64_decode_url(enc, 2, dec, sizeof(dec), false);
    ASSERT(dlen == 1 && dec[0] == 0x41);
}

/* 2-byte input: "AB" (0x41,0x42) -> "QUI=" (std) or "QUI" (url, no pad). */
static void test_two_bytes(void) {
    uint8_t input[] = {0x41, 0x42};
    uint8_t enc[10], dec[10];

    int elen = xylem_base64_encode_std(input, 2, enc, sizeof(enc));
    ASSERT(elen == 4);
    ASSERT(memcmp(enc, "QUI=", 4) == 0);

    int dlen = xylem_base64_decode_std(enc, 4, dec, sizeof(dec));
    ASSERT(dlen == 2);
    ASSERT(dec[0] == 0x41 && dec[1] == 0x42);

    elen = xylem_base64_encode_url(input, 2, enc, sizeof(enc), false);
    ASSERT(elen == 3);
    ASSERT(memcmp(enc, "QUI", 3) == 0);
    dlen = xylem_base64_decode_url(enc, 3, dec, sizeof(dec), false);
    ASSERT(dlen == 2);
    ASSERT(dec[0] == 0x41 && dec[1] == 0x42);
}

/* 3-byte input: full block ("ABC") -> "QUJD", no padding needed. */
static void test_three_bytes(void) {
    uint8_t input[] = {0x41, 0x42, 0x43};
    uint8_t enc[10], dec[10];

    int elen = xylem_base64_encode_std(input, 3, enc, sizeof(enc));
    ASSERT(elen == 4);
    ASSERT(memcmp(enc, "QUJD", 4) == 0);

    int dlen = xylem_base64_decode_std(enc, 4, dec, sizeof(dec));
    ASSERT(dlen == 3);
    ASSERT(memcmp(dec, input, 3) == 0);

    elen = xylem_base64_encode_url(input, 3, enc, sizeof(enc), false);
    ASSERT(elen == 4);
    ASSERT(memcmp(enc, "QUJD", 4) == 0);
    dlen = xylem_base64_decode_url(enc, 4, dec, sizeof(dec), false);
    ASSERT(dlen == 3);
    ASSERT(memcmp(dec, input, 3) == 0);
}

/* Multi-block input: "Hello!" (6 bytes) -> "SGVsbG8h". */
static void test_multi_block(void) {
    uint8_t input[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x21};
    uint8_t enc[20], dec[20];

    int elen = xylem_base64_encode_std(input, 6, enc, sizeof(enc));
    ASSERT(elen == 8);
    ASSERT(memcmp(enc, "SGVsbG8h", 8) == 0);

    int dlen = xylem_base64_decode_std(enc, 8, dec, sizeof(dec));
    ASSERT(dlen == 6);
    ASSERT(memcmp(dec, input, 6) == 0);
}

/* Illegal characters in input should return -1. */
static void test_illegal_characters(void) {
    uint8_t dec[10];

    ASSERT(
        xylem_base64_decode_std((uint8_t*)"AB!D", 4, dec, sizeof(dec)) == -1);
    ASSERT(
        xylem_base64_decode_url((uint8_t*)"AB+D", 4, dec, sizeof(dec), false) ==
        -1);

    /* '_' is valid in URL mode */
    int dlen =
        xylem_base64_decode_url((uint8_t*)"AB_D", 4, dec, sizeof(dec), false);
    ASSERT(dlen >= 0);
}

/* Malformed padding: "Q=Q=", "=QQQ", "Q===" should all be rejected. */
static void test_malformed_padding(void) {
    uint8_t dec[10];

    ASSERT(
        xylem_base64_decode_std((uint8_t*)"Q=Q=", 4, dec, sizeof(dec)) == -1);
    ASSERT(
        xylem_base64_decode_std((uint8_t*)"=QQQ", 4, dec, sizeof(dec)) == -1);
    ASSERT(
        xylem_base64_decode_std((uint8_t*)"Q===", 4, dec, sizeof(dec)) == -1);
}

/* Non-multiple-of-4 input: rejected in std mode, accepted in url no-pad. */
static void test_non_multiple_of_4(void) {
    uint8_t dec[10];

    ASSERT(xylem_base64_decode_std((uint8_t*)"QQ", 2, dec, sizeof(dec)) == -1);
    ASSERT(xylem_base64_decode_std((uint8_t*)"QUI", 3, dec, sizeof(dec)) == -1);

    ASSERT(
        xylem_base64_decode_url((uint8_t*)"QQ", 2, dec, sizeof(dec), false) >=
        0);
    ASSERT(
        xylem_base64_decode_url((uint8_t*)"QUI", 3, dec, sizeof(dec), false) >=
        0);
    ASSERT(
        xylem_base64_decode_url((uint8_t*)"QQ", 2, dec, sizeof(dec), true) ==
        -1);
}

/* Buffer too small for encode or decode should return -1. */
static void test_insufficient_buffer(void) {
    uint8_t input[] = {0x41};
    uint8_t tiny_enc[3];
    uint8_t tiny_dec[2];

    ASSERT(xylem_base64_encode_std(input, 1, tiny_enc, 3) == -1);
    ASSERT(xylem_base64_decode_std((uint8_t*)"QQ==", 4, tiny_dec, 2) == -1);
}

/* Round-trip for input lengths 0..10: encode -> decode recovers original. */
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

/* Extreme byte values: 0x00, 0xFF, 0x80, 0x7F. */
static void test_extreme_bytes(void) {
    uint8_t input[] = {0x00, 0xFF, 0x80, 0x7F};
    uint8_t enc[32], dec[16];

    int elen = xylem_base64_encode_std(input, 4, enc, sizeof(enc));
    ASSERT(elen == 8); /* 4 bytes -> ceil(4/3)*4 = 8 */

    int dlen = xylem_base64_decode_std(enc, elen, dec, sizeof(dec));
    ASSERT(dlen == 4);
    ASSERT(memcmp(input, dec, 4) == 0);
}

/**
 * Reject non-zero padding bits.
 * "QQ==" is valid (low 4 bits of 'Q'=16 are 0000).
 * "QR==" is invalid (low 4 bits of 'R'=17 are 0001 != 0).
 */
static void test_reject_nonzero_padding_bits(void) {
    uint8_t buf[4];

    int len =
        xylem_base64_decode_std((const uint8_t*)"QQ==", 4, buf, sizeof(buf));
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
