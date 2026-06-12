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

#include <stdlib.h>
#include <string.h>

static void _gzip_roundtrip(const uint8_t* in, size_t len, int level) {
    size_t   bound = xylem_gzip_compress_size(len);
    uint8_t* comp  = (uint8_t*)malloc(bound);
    ASSERT(comp != NULL);

    int clen = xylem_gzip_compress(in, len, comp, bound, level);
    ASSERT(clen > 0);
    ASSERT(comp[0] == 0x1f);
    ASSERT(comp[1] == 0x8b);

    uint8_t* dec = (uint8_t*)malloc(len ? len : 1);
    ASSERT(dec != NULL);
    int dlen = xylem_gzip_decompress(comp, (size_t)clen, dec, len ? len : 1);
    ASSERT(dlen == (int)len);
    ASSERT(memcmp(dec, in, len) == 0);

    free(dec);
    free(comp);
}

static void _deflate_roundtrip(const uint8_t* in, size_t len, int level) {
    size_t   bound = xylem_gzip_deflate_size(len);
    uint8_t* comp  = (uint8_t*)malloc(bound);
    ASSERT(comp != NULL);

    int clen = xylem_gzip_deflate(in, len, comp, bound, level);
    ASSERT(clen > 0);

    uint8_t* dec = (uint8_t*)malloc(len ? len : 1);
    ASSERT(dec != NULL);
    int dlen = xylem_gzip_inflate(comp, (size_t)clen, dec, len ? len : 1);
    ASSERT(dlen == (int)len);
    ASSERT(memcmp(dec, in, len) == 0);

    free(dec);
    free(comp);
}

static void test_compress_decompress(void) {
    const char* input = "Hello, gzip world! This is a test string.";
    _gzip_roundtrip((const uint8_t*)input, strlen(input), -1);
}

static void test_deflate_inflate(void) {
    const char* input = "Raw deflate round-trip test data.";
    _deflate_roundtrip((const uint8_t*)input, strlen(input), -1);
}

static void test_empty_input(void) {
    uint8_t empty = 0;
    _gzip_roundtrip(&empty, 0, -1);
    _deflate_roundtrip(&empty, 0, -1);
}

static void test_invalid_data(void) {
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
    uint8_t out[256];

    ASSERT(xylem_gzip_decompress(garbage, sizeof(garbage), out,
                                 sizeof(out)) == -1);
    ASSERT(xylem_gzip_inflate(garbage, sizeof(garbage), out,
                              sizeof(out)) == -1);
}

static void test_large_data(void) {
    size_t   len   = 100000;
    uint8_t* input = (uint8_t*)malloc(len);
    ASSERT(input != NULL);
    for (size_t i = 0; i < len; i++) {
        input[i] = (uint8_t)(i % 251);
    }
    _gzip_roundtrip(input, len, -1);
    free(input);
}

static void test_compression_levels(void) {
    const char* input = "Test all compression levels.";
    for (int level = 0; level <= 9; level++) {
        _gzip_roundtrip((const uint8_t*)input, strlen(input), level);
    }
}

static void test_insufficient_buffer(void) {
    const char* input = "This string needs more than 2 bytes of output.";
    size_t      slen  = strlen(input);
    uint8_t     tiny[2];

    ASSERT(xylem_gzip_compress((const uint8_t*)input, slen, tiny,
                               sizeof(tiny), -1) == -1);
    ASSERT(xylem_gzip_deflate((const uint8_t*)input, slen, tiny,
                              sizeof(tiny), -1) == -1);
}

static void test_null_dst(void) {
    const uint8_t input[] = "test";
    ASSERT(xylem_gzip_compress(input, 4, NULL, 100, -1) == -1);
    ASSERT(xylem_gzip_deflate(input, 4, NULL, 100, -1) == -1);
    ASSERT(xylem_gzip_decompress(input, 4, NULL, 100) == -1);
    ASSERT(xylem_gzip_inflate(input, 4, NULL, 100) == -1);
}

static void test_bound(void) {
    ASSERT(xylem_gzip_compress_size(0) > 0);
    ASSERT(xylem_gzip_compress_size(1024) > 1024);
    ASSERT(xylem_gzip_deflate_size(0) > 0);
    ASSERT(xylem_gzip_deflate_size(1024) > 0);
    ASSERT(xylem_gzip_compress_size(100) > xylem_gzip_deflate_size(100));
}

int main(void) {
    test_compress_decompress();
    test_deflate_inflate();
    test_empty_input();
    test_invalid_data();
    test_large_data();
    test_compression_levels();
    test_insufficient_buffer();
    test_null_dst();
    test_bound();
    return 0;
}
