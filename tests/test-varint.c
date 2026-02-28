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

/* Encode/decode zero. */
static void test_basic_encode_decode(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t val;

    pos = 0;
    ASSERT(xylem_varint_encode(0, buf, sizeof(buf), &pos));
    ASSERT(pos == 1);
    ASSERT(buf[0] == 0);

    pos = 0;
    ASSERT(xylem_varint_decode(buf, sizeof(buf), &pos, &val));
    ASSERT(pos == 1);
    ASSERT(val == 0);
}

/* Single-byte values: 1 and 127. */
static void test_small_values(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t val;

    pos = 0;
    ASSERT(xylem_varint_encode(1, buf, sizeof(buf), &pos));
    ASSERT(pos == 1 && buf[0] == 1);
    pos = 0;
    ASSERT(xylem_varint_decode(buf, sizeof(buf), &pos, &val));
    ASSERT(val == 1);

    pos = 0;
    ASSERT(xylem_varint_encode(127, buf, sizeof(buf), &pos));
    ASSERT(pos == 1 && buf[0] == 127);
    pos = 0;
    ASSERT(xylem_varint_decode(buf, sizeof(buf), &pos, &val));
    ASSERT(val == 127);
}

/* Two-byte encoding: 128 and 16383. */
static void test_two_byte_encoding(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t val;

    pos = 0;
    ASSERT(xylem_varint_encode(128, buf, sizeof(buf), &pos));
    ASSERT(pos == 2 && buf[0] == 0x80 && buf[1] == 0x01);
    pos = 0;
    ASSERT(xylem_varint_decode(buf, sizeof(buf), &pos, &val));
    ASSERT(val == 128);

    pos = 0;
    ASSERT(xylem_varint_encode(16383, buf, sizeof(buf), &pos));
    ASSERT(pos == 2 && buf[0] == 0xFF && buf[1] == 0x7F);
    pos = 0;
    ASSERT(xylem_varint_decode(buf, sizeof(buf), &pos, &val));
    ASSERT(val == 16383);
}

/* Multi-byte: 3-byte and 4-byte boundaries. */
static void test_large_values(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t val;

    pos = 0;
    ASSERT(xylem_varint_encode(16384, buf, sizeof(buf), &pos));
    ASSERT(pos == 3);
    pos = 0;
    ASSERT(xylem_varint_decode(buf, sizeof(buf), &pos, &val));
    ASSERT(val == 16384);

    pos = 0;
    ASSERT(xylem_varint_encode(2097151, buf, sizeof(buf), &pos));
    ASSERT(pos == 3);
    pos = 0;
    ASSERT(xylem_varint_decode(buf, sizeof(buf), &pos, &val));
    ASSERT(val == 2097151);

    pos = 0;
    ASSERT(xylem_varint_encode(2097152, buf, sizeof(buf), &pos));
    ASSERT(pos == 4);
    pos = 0;
    ASSERT(xylem_varint_decode(buf, sizeof(buf), &pos, &val));
    ASSERT(val == 2097152);
}

/* UINT64_MAX requires 10 bytes. */
static void test_max_uint64(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t val;

    pos = 0;
    ASSERT(xylem_varint_encode(UINT64_MAX, buf, sizeof(buf), &pos));
    ASSERT(pos == 10);
    ASSERT(buf[9] == 0x01);

    pos = 0;
    ASSERT(xylem_varint_decode(buf, sizeof(buf), &pos, &val));
    ASSERT(pos == 10);
    ASSERT(val == UINT64_MAX);
}

/* Encode fails when buffer is too small. */
static void test_encode_buffer_too_small(void) {
    uint8_t buf[5];
    size_t  pos;

    pos = 0;
    ASSERT(!xylem_varint_encode(1000000, buf, 2, &pos));
    ASSERT(pos == 0);

    pos = 0;
    ASSERT(xylem_varint_encode(127, buf, 1, &pos));
    ASSERT(pos == 1);

    pos = 0;
    ASSERT(!xylem_varint_encode(128, buf, 1, &pos));
    ASSERT(pos == 0);
}

/* Decode fails when buffer is truncated. */
static void test_decode_buffer_too_small(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t val;

    buf[0] = 0x80;
    buf[1] = 0x01;

    pos = 0;
    ASSERT(!xylem_varint_decode(buf, 1, &pos, &val));
}

/* Encode with invalid starting position. */
static void test_encode_invalid_position(void) {
    uint8_t buf[16];
    size_t  pos;

    pos = 20;
    ASSERT(!xylem_varint_encode(1, buf, 10, &pos));

    pos = 10;
    ASSERT(!xylem_varint_encode(1, buf, 10, &pos));

    pos = 9;
    ASSERT(xylem_varint_encode(1, buf, 10, &pos));
    ASSERT(pos == 10);
}

/* Decode with invalid starting position. */
static void test_decode_invalid_position(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t val;

    pos = 20;
    ASSERT(!xylem_varint_decode(buf, 10, &pos, &val));

    pos = 10;
    ASSERT(!xylem_varint_decode(buf, 10, &pos, &val));

    buf[9] = 0;
    pos = 9;
    ASSERT(xylem_varint_decode(buf, 10, &pos, &val));
    ASSERT(pos == 10);
}

/* Incomplete continuation bytes. */
static void test_decode_incomplete_sequence(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t val;

    memset(buf, 0x80, 9);
    pos = 0;
    ASSERT(!xylem_varint_decode(buf, 9, &pos, &val));
}

/* More than 10 continuation bytes -> reject. */
static void test_decode_too_many_bytes(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t val;

    memset(buf, 0x80, 10);
    buf[9] = 0x7F;
    pos = 0;
    ASSERT(!xylem_varint_decode(buf, 10, &pos, &val));
}

/* NULL buffer or zero bufsize -> fail. */
static void test_encode_null_pointers(void) {
    uint8_t buf[16];
    size_t  pos;

    pos = 0;
    ASSERT(!xylem_varint_encode(1, NULL, 10, &pos));

    pos = 0;
    ASSERT(!xylem_varint_encode(1, buf, 0, &pos));

    /* NULL pos is allowed (fire-and-forget encode). */
    pos = 0;
    ASSERT(xylem_varint_encode(1, buf, 10, NULL));
}

/* Round-trip across a range of values. */
static void test_roundtrip(void) {
    uint8_t  buf[16];
    size_t   pos;
    uint64_t decoded;

    uint64_t values[] = {
        0, 1, 127, 128, 255, 256, 1000, 10000, 65535, 65536,
        1000000, 0xFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL, UINT64_MAX - 1};

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        pos = 0;
        ASSERT(xylem_varint_encode(values[i], buf, sizeof(buf), &pos));
        size_t enc_size = pos;
        pos = 0;
        ASSERT(xylem_varint_decode(buf, enc_size, &pos, &decoded));
        ASSERT(decoded == values[i]);
        ASSERT(pos == enc_size);
    }
}

/* Encode with NULL pos (fire-and-forget). */
static void test_encode_null_pos(void) {
    uint8_t buf[16];

    memset(buf, 0xFF, sizeof(buf));
    ASSERT(xylem_varint_encode(42, buf, sizeof(buf), NULL));
    ASSERT(buf[0] == 42);
    ASSERT(buf[1] == 0xFF);

    memset(buf, 0xFF, sizeof(buf));
    ASSERT(xylem_varint_encode(128, buf, sizeof(buf), NULL));
    ASSERT(buf[0] == 0x80 && buf[1] == 0x01);
    ASSERT(buf[2] == 0xFF);
}

/* xylem_varint_compute returns correct byte counts. */
static void test_compute_size(void) {
    ASSERT(xylem_varint_compute(0) == 1);
    ASSERT(xylem_varint_compute(127) == 1);
    ASSERT(xylem_varint_compute(128) == 2);
    ASSERT(xylem_varint_compute(16383) == 2);
    ASSERT(xylem_varint_compute(16384) == 3);
    ASSERT(xylem_varint_compute(2097151) == 3);
    ASSERT(xylem_varint_compute(2097152) == 4);
    ASSERT(xylem_varint_compute(268435455) == 4);
    ASSERT(xylem_varint_compute(268435456) == 5);
    ASSERT(xylem_varint_compute(UINT64_MAX) == 10);
}

/* Streaming: encode multiple values then decode them back in order. */
static void test_streaming(void) {
    uint8_t  buf[64];
    size_t   enc_pos = 0;
    uint64_t values[] = {0, 1, 127, 128, 16383, 16384, UINT64_MAX};
    size_t   n = sizeof(values) / sizeof(values[0]);

    for (size_t i = 0; i < n; i++) {
        ASSERT(xylem_varint_encode(values[i], buf, sizeof(buf), &enc_pos));
    }

    size_t dec_pos = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t decoded;
        ASSERT(xylem_varint_decode(buf, enc_pos, &dec_pos, &decoded));
        ASSERT(decoded == values[i]);
    }
    ASSERT(dec_pos == enc_pos);
}

/* Streaming: decode past end of buffer fails. */
static void test_streaming_beyond_buffer(void) {
    uint8_t  buf[16];
    size_t   enc_pos = 0;
    uint64_t val;

    ASSERT(xylem_varint_encode(100, buf, sizeof(buf), &enc_pos));
    ASSERT(xylem_varint_encode(200, buf, sizeof(buf), &enc_pos));

    size_t dec_pos = 0;
    ASSERT(xylem_varint_decode(buf, enc_pos, &dec_pos, &val));
    ASSERT(val == 100);
    ASSERT(xylem_varint_decode(buf, enc_pos, &dec_pos, &val));
    ASSERT(val == 200);
    ASSERT(!xylem_varint_decode(buf, enc_pos, &dec_pos, &val));
}

/* Large streaming sequence: 100 values. */
static void test_streaming_large(void) {
    uint8_t buf[1024];
    size_t  enc_pos = 0;

    for (uint64_t i = 0; i < 100; i++) {
        ASSERT(xylem_varint_encode(i * 1000, buf, sizeof(buf), &enc_pos));
    }

    size_t dec_pos = 0;
    for (uint64_t i = 0; i < 100; i++) {
        uint64_t decoded;
        ASSERT(xylem_varint_decode(buf, enc_pos, &dec_pos, &decoded));
        ASSERT(decoded == i * 1000);
    }
    ASSERT(dec_pos == enc_pos);
}

int main(void) {
    test_basic_encode_decode();
    test_small_values();
    test_two_byte_encoding();
    test_large_values();
    test_max_uint64();
    test_encode_buffer_too_small();
    test_decode_buffer_too_small();
    test_encode_invalid_position();
    test_decode_invalid_position();
    test_decode_incomplete_sequence();
    test_decode_too_many_bytes();
    test_encode_null_pointers();
    test_roundtrip();
    test_encode_null_pos();
    test_compute_size();
    test_streaming();
    test_streaming_beyond_buffer();
    test_streaming_large();
    return 0;
}
