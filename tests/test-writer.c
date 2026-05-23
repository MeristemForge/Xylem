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
    uint8_t* data;
    size_t   cap;
    size_t   pos;
    size_t   write_count;
} _mock_sink_t;

static int _mock_write(void* ctx, const void* data, int len) {
    _mock_sink_t* s = (_mock_sink_t*)ctx;
    if (s->pos >= s->cap) {
        return -1;
    }
    size_t avail = s->cap - s->pos;
    size_t n = avail < (size_t)len ? avail : (size_t)len;
    memcpy(s->data + s->pos, data, n);
    s->pos += n;
    s->write_count++;
    return (int)n;
}

static void test_single_write_buffered(void) {
    uint8_t sink_buf[256];
    _mock_sink_t sink = {.data = sink_buf, .cap = sizeof(sink_buf)};
    uint8_t wbuf[64];
    xylem_writer_t wr;
    xylem_writer_init(&wr, &sink, _mock_write, wbuf, sizeof(wbuf));

    int rc = xylem_writer_write(&wr, "hello", 5);
    ASSERT(rc == 0);
    ASSERT(sink.pos == 0);
    ASSERT(sink.write_count == 0);

    rc = xylem_writer_flush(&wr);
    ASSERT(rc == 0);
    ASSERT(sink.pos == 5);
    ASSERT(sink.write_count == 1);
    ASSERT(memcmp(sink_buf, "hello", 5) == 0);

    xylem_writer_deinit(&wr);
}

static void test_multiple_writes_batched(void) {
    uint8_t sink_buf[256];
    _mock_sink_t sink = {.data = sink_buf, .cap = sizeof(sink_buf)};
    uint8_t wbuf[64];
    xylem_writer_t wr;
    xylem_writer_init(&wr, &sink, _mock_write, wbuf, sizeof(wbuf));

    xylem_writer_write(&wr, "aaa", 3);
    xylem_writer_write(&wr, "bbb", 3);
    xylem_writer_write(&wr, "ccc", 3);
    ASSERT(sink.write_count == 0);

    xylem_writer_flush(&wr);
    ASSERT(sink.write_count == 1);
    ASSERT(sink.pos == 9);
    ASSERT(memcmp(sink_buf, "aaabbbccc", 9) == 0);

    xylem_writer_deinit(&wr);
}

static void test_auto_flush_on_full(void) {
    uint8_t sink_buf[256];
    _mock_sink_t sink = {.data = sink_buf, .cap = sizeof(sink_buf)};
    uint8_t wbuf[8];
    xylem_writer_t wr;
    xylem_writer_init(&wr, &sink, _mock_write, wbuf, sizeof(wbuf));

    xylem_writer_write(&wr, "12345", 5);
    ASSERT(sink.write_count == 0);

    /* This exceeds buffer (5+5=10 > 8), triggers flush of "12345". */
    xylem_writer_write(&wr, "67890", 5);
    ASSERT(sink.write_count == 1);
    ASSERT(memcmp(sink_buf, "12345", 5) == 0);

    /* "67890" is now in the buffer. */
    xylem_writer_flush(&wr);
    ASSERT(sink.write_count == 2);
    ASSERT(sink.pos == 10);
    ASSERT(memcmp(sink_buf, "1234567890", 10) == 0);

    xylem_writer_deinit(&wr);
}

static void test_large_write_bypasses_buffer(void) {
    uint8_t sink_buf[256];
    _mock_sink_t sink = {.data = sink_buf, .cap = sizeof(sink_buf)};
    uint8_t wbuf[8];
    xylem_writer_t wr;
    xylem_writer_init(&wr, &sink, _mock_write, wbuf, sizeof(wbuf));

    xylem_writer_write(&wr, "ab", 2);

    /* Write larger than buffer cap=8, bypasses buffer. */
    xylem_writer_write(&wr, "LARGE-DATA!!", 12);
    /* "ab" flushed first, then "LARGE-DATA!!" sent directly. */
    ASSERT(sink.write_count == 2);
    ASSERT(sink.pos == 14);
    ASSERT(memcmp(sink_buf, "abLARGE-DATA!!", 14) == 0);

    xylem_writer_deinit(&wr);
}

static void test_flush_empty(void) {
    uint8_t sink_buf[64];
    _mock_sink_t sink = {.data = sink_buf, .cap = sizeof(sink_buf)};
    uint8_t wbuf[16];
    xylem_writer_t wr;
    xylem_writer_init(&wr, &sink, _mock_write, wbuf, sizeof(wbuf));

    int rc = xylem_writer_flush(&wr);
    ASSERT(rc == 0);
    ASSERT(sink.write_count == 0);

    xylem_writer_deinit(&wr);
}

static void test_write_error_propagates(void) {
    _mock_sink_t sink = {.data = NULL, .cap = 0};
    uint8_t wbuf[8];
    xylem_writer_t wr;
    xylem_writer_init(&wr, &sink, _mock_write, wbuf, sizeof(wbuf));

    /* Buffer 7 bytes (does not trigger flush). */
    int rc = xylem_writer_write(&wr, "1234567", 7);
    ASSERT(rc == 0);

    /* Next write exceeds buffer, triggers flush which fails (cap=0). */
    rc = xylem_writer_write(&wr, "xy", 2);
    ASSERT(rc == -1);

    xylem_writer_deinit(&wr);
}

int main(void) {
    test_single_write_buffered();
    test_multiple_writes_batched();
    test_auto_flush_on_full();
    test_large_write_bypasses_buffer();
    test_flush_empty();
    test_write_error_propagates();
    return 0;
}
