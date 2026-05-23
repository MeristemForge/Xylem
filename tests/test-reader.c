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

typedef struct {
    const uint8_t* data;
    size_t         len;
    size_t         pos;
    size_t         chunk_size;
} _mock_stream_t;

static int _mock_read(void* ctx, void* buf, int len) {
    _mock_stream_t* s = (_mock_stream_t*)ctx;
    if (s->pos >= s->len) {
        return 0;
    }
    size_t avail = s->len - s->pos;
    size_t n = avail < (size_t)len ? avail : (size_t)len;
    if (s->chunk_size > 0 && n > s->chunk_size) {
        n = s->chunk_size;
    }
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return (int)n;
}

static void test_read(void) {
    const uint8_t data[] = "hello world";
    _mock_stream_t stream = {
        .data = data, .len = 11, .pos = 0, .chunk_size = 0};
    uint8_t rbuf[64];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    uint8_t out[16];
    int n = xylem_reader_read(&rd, out, sizeof(out));
    ASSERT(n == 11);
    ASSERT(memcmp(out, "hello world", 11) == 0);

    /* EOF */
    n = xylem_reader_read(&rd, out, sizeof(out));
    ASSERT(n == 0);

    xylem_reader_deinit(&rd);
}

static void test_read_partial_chunks(void) {
    const uint8_t data[] = "abcdefghij";
    _mock_stream_t stream = {
        .data = data, .len = 10, .pos = 0, .chunk_size = 3};
    uint8_t rbuf[8];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    /* First read fills buffer (up to chunk_size=3), returns what's available. */
    uint8_t out[10];
    int n = xylem_reader_read(&rd, out, 10);
    ASSERT(n > 0 && n <= 10);

    xylem_reader_deinit(&rd);
}

static void test_read_full(void) {
    const uint8_t data[] = "0123456789ABCDEF";
    _mock_stream_t stream = {
        .data = data, .len = 16, .pos = 0, .chunk_size = 3};
    uint8_t rbuf[8];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    uint8_t out[16];
    int rc = xylem_reader_read_full(&rd, out, 16);
    ASSERT(rc == 0);
    ASSERT(memcmp(out, "0123456789ABCDEF", 16) == 0);

    xylem_reader_deinit(&rd);
}

static void test_read_full_eof(void) {
    const uint8_t data[] = "short";
    _mock_stream_t stream = {
        .data = data, .len = 5, .pos = 0, .chunk_size = 0};
    uint8_t rbuf[32];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    uint8_t out[10];
    int rc = xylem_reader_read_full(&rd, out, 10);
    ASSERT(rc == -1);

    xylem_reader_deinit(&rd);
}

static void test_read_until(void) {
    const uint8_t data[] = "line1\nline2\nline3";
    _mock_stream_t stream = {
        .data = data, .len = 17, .pos = 0, .chunk_size = 4};
    uint8_t rbuf[8];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    uint8_t out[32];
    int n = xylem_reader_read_until(&rd, '\n', out, sizeof(out));
    ASSERT(n == 6);
    ASSERT(memcmp(out, "line1\n", 6) == 0);

    n = xylem_reader_read_until(&rd, '\n', out, sizeof(out));
    ASSERT(n == 6);
    ASSERT(memcmp(out, "line2\n", 6) == 0);

    xylem_reader_deinit(&rd);
}

static void test_read_until_overflow(void) {
    const uint8_t data[] = "a very long line without delimiter";
    _mock_stream_t stream = {
        .data = data, .len = 34, .pos = 0, .chunk_size = 0};
    uint8_t rbuf[64];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    uint8_t out[8];
    int n = xylem_reader_read_until(&rd, '\n', out, sizeof(out));
    ASSERT(n == -1);

    xylem_reader_deinit(&rd);
}

static void test_read_until_eof(void) {
    const uint8_t data[] = "no delimiter";
    _mock_stream_t stream = {
        .data = data, .len = 12, .pos = 0, .chunk_size = 0};
    uint8_t rbuf[64];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    uint8_t out[64];
    int n = xylem_reader_read_until(&rd, '\n', out, sizeof(out));
    ASSERT(n == -1);

    xylem_reader_deinit(&rd);
}

static void test_peek(void) {
    const uint8_t data[] = "ABCDEF";
    _mock_stream_t stream = {
        .data = data, .len = 6, .pos = 0, .chunk_size = 2};
    uint8_t rbuf[16];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    uint8_t out[4];
    int rc = xylem_reader_peek(&rd, out, 4);
    ASSERT(rc == 0);
    ASSERT(memcmp(out, "ABCD", 4) == 0);

    /* Peek again returns same data. */
    rc = xylem_reader_peek(&rd, out, 4);
    ASSERT(rc == 0);
    ASSERT(memcmp(out, "ABCD", 4) == 0);

    /* Read consumes the peeked data. */
    uint8_t consumed[4];
    int n = xylem_reader_read(&rd, consumed, 4);
    ASSERT(n == 4);
    ASSERT(memcmp(consumed, "ABCD", 4) == 0);

    /* Next read gives remaining data. */
    n = xylem_reader_read(&rd, consumed, 4);
    ASSERT(n > 0);
    ASSERT(memcmp(consumed, "EF", (size_t)n) == 0);

    xylem_reader_deinit(&rd);
}

static void test_peek_exceeds_cap(void) {
    const uint8_t data[] = "AB";
    _mock_stream_t stream = {
        .data = data, .len = 2, .pos = 0, .chunk_size = 0};
    uint8_t rbuf[4];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    uint8_t out[8];
    int rc = xylem_reader_peek(&rd, out, 8);
    ASSERT(rc == -1);

    xylem_reader_deinit(&rd);
}

static void test_mixed_read_full_and_until(void) {
    /* Simulate HTTP-like: headers (line-based) then body (fixed length). */
    const uint8_t data[] = "Content-Length: 5\r\n\r\nhello";
    _mock_stream_t stream = {
        .data = data, .len = 26, .pos = 0, .chunk_size = 7};
    uint8_t rbuf[16];
    xylem_reader_t rd;
    xylem_reader_init(&rd, &stream, _mock_read, rbuf, sizeof(rbuf));

    /* Read header line. */
    uint8_t line[64];
    int n = xylem_reader_read_until(&rd, '\n', line, sizeof(line));
    ASSERT(n == 19);
    ASSERT(memcmp(line, "Content-Length: 5\r\n", 19) == 0);

    /* Read empty line. */
    n = xylem_reader_read_until(&rd, '\n', line, sizeof(line));
    ASSERT(n == 2);
    ASSERT(memcmp(line, "\r\n", 2) == 0);

    /* Read body (fixed 5 bytes). */
    uint8_t body[8];
    int rc = xylem_reader_read_full(&rd, body, 5);
    ASSERT(rc == 0);
    ASSERT(memcmp(body, "hello", 5) == 0);

    xylem_reader_deinit(&rd);
}

int main(void) {
    test_read();
    test_read_partial_chunks();
    test_read_full();
    test_read_full_eof();
    test_read_until();
    test_read_until_overflow();
    test_read_until_eof();
    test_peek();
    test_peek_exceeds_cap();
    test_mixed_read_full_and_until();
    return 0;
}
