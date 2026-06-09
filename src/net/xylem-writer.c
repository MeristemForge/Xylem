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

#include "xylem/net/xylem-tcp.h"
#include "xylem/net/xylem-tls.h"
#include "xylem/net/xylem-uds.h"
#include "xylem/net/xylem-rudp.h"
#include "xylem/net/xylem-mux.h"

#include "runtime/precond.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_BUF_SIZE 4096

typedef int (*_writer_fn)(void* ctx, const void* data, int len);

struct xylem_writer_s {
    void*      ctx;
    _writer_fn write_fn;
    int        buflen;
    int        w;
    uint8_t    buf[];
};

static _writer_fn _writer_resolve_transport(xylem_writer_transport_t transport) {
    switch (transport) {
    case XYLEM_WRITER_TCP:         return (_writer_fn)xylem_tcp_write;
    case XYLEM_WRITER_TLS:         return (_writer_fn)xylem_tls_write;
    case XYLEM_WRITER_UDS:         return (_writer_fn)xylem_uds_write;
    case XYLEM_WRITER_RUDP_STREAM: return (_writer_fn)xylem_rudp_write;
    case XYLEM_WRITER_MUX:         return (_writer_fn)xylem_mux_write;
    default:                       return NULL;
    }
}

xylem_writer_t* xylem_writer_create(
    void*                    conn,
    xylem_writer_transport_t transport,
    int                      size) {
    if (size <= 0) {
        size = DEFAULT_BUF_SIZE;
    }

    _writer_fn fn = _writer_resolve_transport(transport);
    if (!fn) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("writer", "xylem_writer_create");

    xylem_writer_t* wr = (xylem_writer_t*)calloc(
        1, sizeof(xylem_writer_t) + (size_t)size);
    if (!wr) {
        return NULL;
    }

    wr->ctx      = conn;
    wr->write_fn = fn;
    wr->buflen   = size;
    return wr;
}

void xylem_writer_destroy(xylem_writer_t* wr) {
    RUNTIME_REQUIRE_COROUTINE("writer", "xylem_writer_destroy");

    xylem_writer_flush(wr);
    free(wr);
}

int xylem_writer_flush(xylem_writer_t* wr) {
    RUNTIME_REQUIRE_COROUTINE("writer", "xylem_writer_flush");

    if (wr->w == 0) {
        return 0;
    }
    int rc = wr->write_fn(wr->ctx, wr->buf, wr->w);
    wr->w = 0;
    return rc;
}

int xylem_writer_write(xylem_writer_t* wr, const void* data, int len) {
    RUNTIME_REQUIRE_COROUTINE("writer", "xylem_writer_write");

    if (len >= wr->buflen) {
        if (xylem_writer_flush(wr) != 0) {
            return -1;
        }
        return wr->write_fn(wr->ctx, data, len);
    }

    if (wr->w + len > wr->buflen) {
        if (xylem_writer_flush(wr) != 0) {
            return -1;
        }
    }

    memcpy(wr->buf + wr->w, data, (size_t)len);
    wr->w += len;
    return 0;
}
