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

#include "xylem/net/xylem-reader.h"

#include <string.h>

static int _reader_drain(xylem_reader_t* rd, void* dst, int len) {
    int buffered = rd->w - rd->r;
    int n = buffered < len ? buffered : len;
    if (n > 0) {
        memcpy(dst, rd->buf + rd->r, (size_t)n);
        rd->r += n;
    }
    return n;
}

static int _reader_fill(xylem_reader_t* rd) {
    if (rd->r >= rd->w) {
        rd->r = 0;
        rd->w = 0;
    } else if (rd->r > 0) {
        rd->w -= rd->r;
        memmove(rd->buf, rd->buf + rd->r, (size_t)rd->w);
        rd->r = 0;
    }

    int n = rd->read_fn(rd->ctx, rd->buf + rd->w, rd->cap - rd->w);
    if (n > 0) {
        rd->w += n;
    }
    return n;
}

void xylem_reader_init(
    xylem_reader_t*   rd,
    void*             ctx,
    xylem_reader_fn_t read_fn,
    void*             buf,
    int               cap) {
    rd->ctx     = ctx;
    rd->read_fn = read_fn;
    rd->buf     = (uint8_t*)buf;
    rd->cap     = cap;
    rd->r       = 0;
    rd->w       = 0;
}

void xylem_reader_deinit(xylem_reader_t* rd) {
    memset(rd, 0, sizeof(*rd));
}

int xylem_reader_read(xylem_reader_t* rd, void* buf, int len) {
    int n = _reader_drain(rd, buf, len);
    if (n > 0) {
        return n;
    }

    if (len >= rd->cap) {
        return rd->read_fn(rd->ctx, buf, len);
    }

    int rc = _reader_fill(rd);
    if (rc <= 0) {
        return rc;
    }

    return _reader_drain(rd, buf, len);
}

int xylem_reader_read_full(xylem_reader_t* rd, void* buf, int len) {
    uint8_t* dst = (uint8_t*)buf;
    int      rem = len;

    int drained = _reader_drain(rd, dst, rem);
    dst += drained;
    rem -= drained;

    while (rem > 0) {
        int n = rd->read_fn(rd->ctx, dst, rem);
        if (n <= 0) {
            return -1;
        }
        dst += n;
        rem -= n;
    }
    return 0;
}

int xylem_reader_read_until(
    xylem_reader_t* rd,
    uint8_t         delim,
    void*           buf,
    int             len) {
    uint8_t* dst = (uint8_t*)buf;
    int      pos = 0;

    for (;;) {
        int      avail = rd->w - rd->r;
        uint8_t* found = (uint8_t*)memchr(rd->buf + rd->r, delim, (size_t)avail);

        if (found) {
            int n = (int)(found - (rd->buf + rd->r)) + 1;
            if (pos + n > len) {
                return -1;
            }
            memcpy(dst + pos, rd->buf + rd->r, (size_t)n);
            rd->r += n;
            return pos + n;
        }

        if (pos + avail > len) {
            return -1;
        }
        memcpy(dst + pos, rd->buf + rd->r, (size_t)avail);
        pos += avail;
        rd->r = 0;
        rd->w = 0;

        int n = rd->read_fn(rd->ctx, rd->buf, rd->cap);
        if (n <= 0) {
            return -1;
        }
        rd->w = n;
    }
}

int xylem_reader_peek(xylem_reader_t* rd, void* buf, int len) {
    if (len > rd->cap) {
        return -1;
    }

    while (rd->w - rd->r < len) {
        int rc = _reader_fill(rd);
        if (rc <= 0) {
            return -1;
        }
    }

    memcpy(buf, rd->buf + rd->r, (size_t)len);
    return 0;
}
