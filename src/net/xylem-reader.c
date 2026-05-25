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
#include "xylem/net/xylem-tcp.h"
#include "xylem/net/xylem-tls.h"
#include "xylem/net/xylem-uds.h"
#include "xylem/net/xylem-rudp.h"
#include "xylem/net/xylem-mux.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_BUF_SIZE 4096

typedef int (*_reader_fn)(void* ctx, void* buf, int len);

struct xylem_reader_s {
    void*      ctx;
    _reader_fn read_fn;
    int        buflen;
    int        r;
    int        w;
    uint8_t    buf[];
};

static _reader_fn _reader_resolve_transport(xylem_reader_transport_t transport) {
    switch (transport) {
    case XYLEM_READER_TCP:         return (_reader_fn)xylem_tcp_read;
    case XYLEM_READER_TLS:         return (_reader_fn)xylem_tls_read;
    case XYLEM_READER_UDS:         return (_reader_fn)xylem_uds_read;
    case XYLEM_READER_RUDP_STREAM: return (_reader_fn)xylem_rudp_read;
    case XYLEM_READER_MUX:         return (_reader_fn)xylem_mux_read;
    default:                       return NULL;
    }
}

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

    int n = rd->read_fn(rd->ctx, rd->buf + rd->w, rd->buflen - rd->w);
    if (n > 0) {
        rd->w += n;
    }
    return n;
}

xylem_reader_t* xylem_reader_create(
    void*                    conn,
    xylem_reader_transport_t transport,
    int                      size) {
    if (size <= 0) {
        size = DEFAULT_BUF_SIZE;
    }

    _reader_fn fn = _reader_resolve_transport(transport);
    if (!fn) {
        return NULL;
    }

    xylem_reader_t* rd = (xylem_reader_t*)calloc(
        1, sizeof(xylem_reader_t) + (size_t)size);
    if (!rd) {
        return NULL;
    }

    rd->ctx     = conn;
    rd->read_fn = fn;
    rd->buflen  = size;
    return rd;
}

void xylem_reader_destroy(xylem_reader_t* rd) {
    free(rd);
}

int xylem_reader_read(xylem_reader_t* rd, void* buf, int len) {
    int n = _reader_drain(rd, buf, len);
    if (n > 0) {
        return n;
    }

    if (len >= rd->buflen) {
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

        int n = rd->read_fn(rd->ctx, rd->buf, rd->buflen);
        if (n <= 0) {
            return -1;
        }
        rd->w = n;
    }
}

int xylem_reader_peek(xylem_reader_t* rd, void* buf, int len) {
    if (len > rd->buflen) {
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
