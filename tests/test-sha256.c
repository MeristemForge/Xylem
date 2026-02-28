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

static void _hex_digest(const uint8_t digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[64] = '\0';
}

/* NIST FIPS 180-4: empty string */
static void test_empty(void) {
    xylem_sha256_t* ctx = xylem_sha256_create();
    ASSERT(ctx);
    uint8_t digest[32];
    xylem_sha256_final(ctx, digest);

    char hex[65];
    _hex_digest(digest, hex);
    ASSERT(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    xylem_sha256_destroy(ctx);
}

/* NIST FIPS 180-4: "abc" */
static void test_abc(void) {
    xylem_sha256_t* ctx = xylem_sha256_create();
    ASSERT(ctx);
    const uint8_t msg[] = "abc";
    xylem_sha256_update(ctx, msg, 3);
    uint8_t digest[32];
    xylem_sha256_final(ctx, digest);

    char hex[65];
    _hex_digest(digest, hex);
    ASSERT(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    xylem_sha256_destroy(ctx);
}

/* NIST FIPS 180-4: 448-bit message */
static void test_448bit(void) {
    xylem_sha256_t* ctx = xylem_sha256_create();
    ASSERT(ctx);
    const uint8_t msg[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    xylem_sha256_update(ctx, msg, 56);
    uint8_t digest[32];
    xylem_sha256_final(ctx, digest);

    char hex[65];
    _hex_digest(digest, hex);
    ASSERT(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    xylem_sha256_destroy(ctx);
}

/* NIST: one million 'a' characters */
static void test_million_a(void) {
    xylem_sha256_t* ctx = xylem_sha256_create();
    ASSERT(ctx);
    uint8_t block[1000];
    memset(block, 'a', 1000);
    for (int i = 0; i < 1000; i++) {
        xylem_sha256_update(ctx, block, 1000);
    }
    uint8_t digest[32];
    xylem_sha256_final(ctx, digest);

    char hex[65];
    _hex_digest(digest, hex);
    ASSERT(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    xylem_sha256_destroy(ctx);
}

/* incremental: feed "abc" one byte at a time */
static void test_incremental(void) {
    xylem_sha256_t* ctx = xylem_sha256_create();
    ASSERT(ctx);
    xylem_sha256_update(ctx, (const uint8_t*)"a", 1);
    xylem_sha256_update(ctx, (const uint8_t*)"b", 1);
    xylem_sha256_update(ctx, (const uint8_t*)"c", 1);
    uint8_t digest[32];
    xylem_sha256_final(ctx, digest);

    char hex[65];
    _hex_digest(digest, hex);
    ASSERT(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    xylem_sha256_destroy(ctx);
}

/* 55 bytes: max single-block message, verify via byte-by-byte re-hash */
static void test_55_bytes(void) {
    xylem_sha256_t* ctx = xylem_sha256_create();
    ASSERT(ctx);
    const uint8_t msg[] = "0123456789012345678901234567890123456789012345678901234";
    xylem_sha256_update(ctx, msg, 55);
    uint8_t digest[32];
    xylem_sha256_final(ctx, digest);

    xylem_sha256_t* ctx2 = xylem_sha256_create();
    ASSERT(ctx2);
    for (int i = 0; i < 55; i++) {
        xylem_sha256_update(ctx2, &msg[i], 1);
    }
    uint8_t digest2[32];
    xylem_sha256_final(ctx2, digest2);
    ASSERT(memcmp(digest, digest2, 32) == 0);
    xylem_sha256_destroy(ctx);
    xylem_sha256_destroy(ctx2);
}

int main(void) {
    test_empty();
    test_abc();
    test_448bit();
    test_incremental();
    test_55_bytes();
    test_million_a();
    return 0;
}
