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

#include "xylem/net/xylem-writer.h"

#include <string.h>

void xylem_writer_init(
    xylem_writer_t*   wr,
    void*             ctx,
    xylem_writer_fn_t write_fn,
    void*             buf,
    int               cap) {
    wr->ctx      = ctx;
    wr->write_fn = write_fn;
    wr->buf      = (uint8_t*)buf;
    wr->cap      = cap;
    wr->w        = 0;
}

void xylem_writer_deinit(xylem_writer_t* wr) {
    memset(wr, 0, sizeof(*wr));
}

int xylem_writer_flush(xylem_writer_t* wr) {
    if (wr->w == 0) {
        return 0;
    }
    int rc = wr->write_fn(wr->ctx, wr->buf, wr->w);
    wr->w = 0;
    return rc;
}

int xylem_writer_write(xylem_writer_t* wr, const void* data, int len) {
    if (len >= wr->cap) {
        if (xylem_writer_flush(wr) != 0) {
            return -1;
        }
        return wr->write_fn(wr->ctx, data, len);
    }

    if (wr->w + len > wr->cap) {
        if (xylem_writer_flush(wr) != 0) {
            return -1;
        }
    }

    memcpy(wr->buf + wr->w, data, (size_t)len);
    wr->w += len;
    return 0;
}
