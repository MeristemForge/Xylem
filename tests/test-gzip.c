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

/* Round-trip: gzip compress then decompress recovers original data. */
static void test_compress_decompress(void) {
    const char *input = "Hello, gzip world! This is a test string.";
    size_t slen = strlen(input);
    size_t bound = xylem_gzip_compress_bound(slen);

    uint8_t *compressed = (uint8_t *)malloc(bound);
    ASSERT(compressed != NULL);

    int clen = xylem_gzip_compress((const uint8_t *)input, slen, compressed,
                                   bound, -1);
    ASSERT(clen > 0);

    /* Verify gzip magic bytes. */
    ASSERT(compressed[0] == 0x1f);
    ASSERT(compressed[1] == 0x8b);

    uint8_t decompressed[256];
    int dlen = xylem_gzip_decompress(compressed, (size_t)clen, decompressed,
                                     sizeof(decompressed));
    ASSERT(dlen == (int)slen);
    ASSERT(memcmp(decompressed, input, slen) == 0);

    free(compressed);
}

/* Round-trip: raw deflate then inflate recovers original data. */
static void test_deflate_inflate(void) {
    const char *input = "Raw deflate round-trip test data.";
    size_t slen = strlen(input);
    size_t bound = xylem_gzip_deflate_bound(slen);

    uint8_t *deflated = (uint8_t *)malloc(bound);
    ASSERT(deflated != NULL);

    int clen = xylem_gzip_deflate((const uint8_t *)input, slen, deflated,
                                  bound, -1);
    ASSERT(clen > 0);

    uint8_t inflated[256];
    int dlen = xylem_gzip_inflate(deflated, (size_t)clen, inflated,
                                  sizeof(inflated));
    ASSERT(dlen == (int)slen);
    ASSERT(memcmp(inflated, input, slen) == 0);

    free(deflated);
}

/* Empty input produces a valid gzip stream that decompresses to nothing. */
static void test_empty_input(void) {
    size_t bound = xylem_gzip_compress_bound(0);
    uint8_t *compressed = (uint8_t *)malloc(bound);
    ASSERT(compressed != NULL);

    uint8_t empty = 0;
    int clen = xylem_gzip_compress(&empty, 0, compressed, bound, -1);
    ASSERT(clen > 0);

    uint8_t decompressed[1];
    int dlen = xylem_gzip_decompress(compressed, (size_t)clen, decompressed,
                                     sizeof(decompressed));
    ASSERT(dlen == 0);

    /* Raw deflate/inflate with empty input. */
    size_t dbound = xylem_gzip_deflate_bound(0);
    uint8_t *deflated = (uint8_t *)malloc(dbound);
    ASSERT(deflated != NULL);

    int dclen = xylem_gzip_deflate(&empty, 0, deflated, dbound, -1);
    ASSERT(dclen > 0);

    uint8_t inflated[1];
    int ilen = xylem_gzip_inflate(deflated, (size_t)dclen, inflated,
                                  sizeof(inflated));
    ASSERT(ilen == 0);

    free(deflated);
    free(compressed);
}


/* Decompressing garbage data returns -1. */
static void test_invalid_data(void) {
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
    uint8_t out[256];

    ASSERT(xylem_gzip_decompress(garbage, sizeof(garbage), out,
                                 sizeof(out)) == -1);
    ASSERT(xylem_gzip_inflate(garbage, sizeof(garbage), out,
                              sizeof(out)) == -1);
}

/* Large data round-trip through gzip compress/decompress. */
static void test_large_data(void) {
    size_t len = 100000;
    uint8_t *input = (uint8_t *)malloc(len);
    ASSERT(input != NULL);
    for (size_t i = 0; i < len; i++) {
        input[i] = (uint8_t)(i % 251);
    }

    size_t bound = xylem_gzip_compress_bound(len);
    uint8_t *compressed = (uint8_t *)malloc(bound);
    ASSERT(compressed != NULL);

    int clen = xylem_gzip_compress(input, len, compressed, bound, -1);
    ASSERT(clen > 0);

    uint8_t *decompressed = (uint8_t *)malloc(len);
    ASSERT(decompressed != NULL);

    int dlen = xylem_gzip_decompress(compressed, (size_t)clen, decompressed,
                                     len);
    ASSERT(dlen == (int)len);
    ASSERT(memcmp(decompressed, input, len) == 0);

    free(decompressed);
    free(compressed);
    free(input);
}

/* Compression levels 0 through 9 all produce valid output. */
static void test_compression_levels(void) {
    const char *input = "Test all compression levels.";
    size_t slen = strlen(input);
    size_t bound = xylem_gzip_compress_bound(slen);
    uint8_t compressed[256];
    ASSERT(bound <= sizeof(compressed));

    for (int level = 0; level <= 9; level++) {
        int clen = xylem_gzip_compress((const uint8_t *)input, slen,
                                       compressed, sizeof(compressed), level);
        ASSERT(clen > 0);

        uint8_t decompressed[256];
        int dlen = xylem_gzip_decompress(compressed, (size_t)clen,
                                         decompressed, sizeof(decompressed));
        ASSERT(dlen == (int)slen);
        ASSERT(memcmp(decompressed, input, slen) == 0);
    }
}

/* Insufficient output buffer returns -1. */
static void test_insufficient_buffer(void) {
    const char *input = "This string needs more than 2 bytes of output.";
    size_t slen = strlen(input);

    uint8_t tiny[2];
    ASSERT(xylem_gzip_compress((const uint8_t *)input, slen, tiny,
                               sizeof(tiny), -1) == -1);
    ASSERT(xylem_gzip_deflate((const uint8_t *)input, slen, tiny,
                              sizeof(tiny), -1) == -1);
}

/* NULL dst returns -1. */
static void test_null_dst(void) {
    const uint8_t input[] = "test";
    ASSERT(xylem_gzip_compress(input, 4, NULL, 100, -1) == -1);
    ASSERT(xylem_gzip_deflate(input, 4, NULL, 100, -1) == -1);
    ASSERT(xylem_gzip_decompress(input, 4, NULL, 100) == -1);
    ASSERT(xylem_gzip_inflate(input, 4, NULL, 100) == -1);
}

/* Bound functions return non-zero for any input. */
static void test_bound(void) {
    ASSERT(xylem_gzip_compress_bound(0) > 0);
    ASSERT(xylem_gzip_compress_bound(1024) > 1024);
    ASSERT(xylem_gzip_deflate_bound(0) > 0);
    ASSERT(xylem_gzip_deflate_bound(1024) > 0);
    /* gzip bound is always larger than deflate bound by the overhead. */
    ASSERT(xylem_gzip_compress_bound(100) > xylem_gzip_deflate_bound(100));
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
