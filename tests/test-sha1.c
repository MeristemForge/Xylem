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

static void _check(const uint8_t digest[20], const char* expected) {
    static const char hex[] = "0123456789abcdef";
    char out[41];
    for (int i = 0; i < 20; i++) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[40] = '\0';
    ASSERT(strcmp(out, expected) == 0);
}

static void test_abc(void) {
    xylem_sha1_t* ctx = xylem_sha1_create();
    ASSERT(ctx);
    xylem_sha1_update(ctx, (const uint8_t*)"abc", 3);
    uint8_t digest[20];
    xylem_sha1_final(ctx, digest);
    _check(digest, "a9993e364706816aba3e25717850c26c9cd0d89d");
    xylem_sha1_destroy(ctx);
}

static void test_448bit(void) {
    xylem_sha1_t* ctx = xylem_sha1_create();
    ASSERT(ctx);
    const uint8_t msg[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    xylem_sha1_update(ctx, msg, 56);
    uint8_t digest[20];
    xylem_sha1_final(ctx, digest);
    _check(digest, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    xylem_sha1_destroy(ctx);
}

static void test_million_a(void) {
    xylem_sha1_t* ctx = xylem_sha1_create();
    ASSERT(ctx);
    uint8_t block[1000];
    memset(block, 'a', 1000);
    for (int i = 0; i < 1000; i++) {
        xylem_sha1_update(ctx, block, 1000);
    }
    uint8_t digest[20];
    xylem_sha1_final(ctx, digest);
    _check(digest, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    xylem_sha1_destroy(ctx);
}

static void test_empty(void) {
    xylem_sha1_t* ctx = xylem_sha1_create();
    ASSERT(ctx);
    uint8_t digest[20];
    xylem_sha1_final(ctx, digest);
    _check(digest, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    xylem_sha1_destroy(ctx);
}

static void test_55_bytes(void) {
    const uint8_t msg[] =
        "0123456789012345678901234567890123456789012345678901234";

    xylem_sha1_t* ctx = xylem_sha1_create();
    ASSERT(ctx);
    xylem_sha1_update(ctx, msg, 55);
    uint8_t digest[20];
    xylem_sha1_final(ctx, digest);

    xylem_sha1_t* ctx2 = xylem_sha1_create();
    ASSERT(ctx2);
    for (int i = 0; i < 55; i++) {
        xylem_sha1_update(ctx2, &msg[i], 1);
    }
    uint8_t digest2[20];
    xylem_sha1_final(ctx2, digest2);

    ASSERT(memcmp(digest, digest2, 20) == 0);
    xylem_sha1_destroy(ctx);
    xylem_sha1_destroy(ctx2);
}

int main(void) {
    test_empty();
    test_abc();
    test_448bit();
    test_55_bytes();
    test_million_a();
    return 0;
}
