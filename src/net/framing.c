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

#include "net/framing.h"

#include "xylem/encoding/xylem-varint.h"
#include "xylem/xylem-logger.h"

#include <stdlib.h>
#include <string.h>

static int _framing_read_exact(framing_t* r, void* buf, size_t len) {
    char*  ptr = (char*)buf;
    size_t rem = len;

    while (rem > 0) {
        size_t avail = r->read_buf_len - r->read_buf_pos;
        if (avail > 0) {
            size_t copy = avail < rem ? avail : rem;
            memcpy(ptr, r->read_buf + r->read_buf_pos, copy);
            r->read_buf_pos += copy;
            ptr += copy;
            rem -= copy;
            continue;
        }

        r->read_buf_pos = 0;
        r->read_buf_len = 0;

        int64_t n = r->recv_fn(r->ctx, r->read_buf, r->read_buf_cap);
        if (n <= 0) {
            return -1;
        }
        r->read_buf_len = (size_t)n;
    }
    return 0;
}

static int64_t
_framing_recv_fixed(framing_t* r, const xylem_framing_opts_t* opts,
            void* buf, size_t len) {
    size_t frame_len = opts->fixed.len;
    if (frame_len > len) {
        xylem_loge("fd=%d recv: fixed frame %zu exceeds buffer %zu",
                   r->fd, frame_len, len);
        return -1;
    }
    if (_framing_read_exact(r, buf, frame_len) != 0) {
        return -1;
    }
    return (int64_t)frame_len;
}

static int64_t
_framing_recv_lenfield_fixint(framing_t* r, const xylem_framing_opts_t* opts,
                              void* buf, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = opts->lenfield_fixint.header_size;

    if (hdr_sz > sizeof(hdr)) {
        xylem_loge("fd=%d recv: header_size %u exceeds limit",
                   r->fd, hdr_sz);
        return -1;
    }

    if (_framing_read_exact(r, hdr, hdr_sz) != 0) {
        return -1;
    }

    uint64_t body_len = 0;
    uint8_t* field    = hdr + opts->lenfield_fixint.field_offset;

    if (opts->lenfield_fixint.big_endian) {
        for (uint32_t i = 0; i < opts->lenfield_fixint.field_size; i++) {
            body_len = (body_len << 8) | field[i];
        }
    } else {
        for (uint32_t i = 0; i < opts->lenfield_fixint.field_size; i++) {
            body_len |= (uint64_t)field[i] << (i * 8);
        }
    }

    int64_t adjusted = (int64_t)body_len + opts->lenfield_fixint.adjustment;
    if (adjusted < 0) {
        xylem_loge("fd=%d recv: negative payload length", r->fd);
        return -1;
    }

    size_t payload_len = (size_t)adjusted;
    if (payload_len > len) {
        xylem_loge("fd=%d recv: payload %zu exceeds buffer %zu",
                   r->fd, payload_len, len);
        return -1;
    }

    if (payload_len > 0 && _framing_read_exact(r, buf, payload_len) != 0) {
        return -1;
    }
    return (int64_t)payload_len;
}

static int64_t
_framing_recv_lenfield_varint(framing_t* r, const xylem_framing_opts_t* opts,
                              void* buf, size_t len) {
    uint32_t prefix_sz = opts->lenfield_varint.prefix_size;
    if (prefix_sz > 0) {
        uint8_t prefix[16];
        if (prefix_sz > sizeof(prefix)) {
            xylem_loge("fd=%d recv: prefix_size %u exceeds limit",
                       r->fd, prefix_sz);
            return -1;
        }
        if (_framing_read_exact(r, prefix, prefix_sz) != 0) {
            return -1;
        }
    }

    uint8_t vbuf[10];
    size_t  vlen = 0;
    for (;;) {
        if (_framing_read_exact(r, &vbuf[vlen], 1) != 0) {
            return -1;
        }
        if (!(vbuf[vlen++] & 0x80)) {
            break;
        }
        if (vlen >= sizeof(vbuf)) {
            xylem_loge("fd=%d recv: varint overflow", r->fd);
            return -1;
        }
    }

    uint64_t body_len = 0;
    size_t   vpos     = 0;
    if (!xylem_varint_decode(vbuf, vlen, &vpos, &body_len)) {
        xylem_loge("fd=%d recv: varint decode failed", r->fd);
        return -1;
    }

    int64_t adjusted = (int64_t)body_len + opts->lenfield_varint.adjustment;
    if (adjusted < 0) {
        xylem_loge("fd=%d recv: negative payload length", r->fd);
        return -1;
    }

    size_t payload_len = (size_t)adjusted;
    if (payload_len > len) {
        xylem_loge("fd=%d recv: payload %zu exceeds buffer %zu",
                   r->fd, payload_len, len);
        return -1;
    }

    if (payload_len > 0 && _framing_read_exact(r, buf, payload_len) != 0) {
        return -1;
    }
    return (int64_t)payload_len;
}

static int64_t
_framing_recv_delimiter(framing_t* r, const xylem_framing_opts_t* opts,
               void* buf, size_t len) {
    const char* delim     = opts->delimiter.delim;
    size_t      delim_len = opts->delimiter.delim_len;
    if (delim_len == 0) {
        delim_len = strlen(delim);
    }

    char*  dst = (char*)buf;
    size_t pos = 0;

    while (pos < len) {
        size_t avail = r->read_buf_len - r->read_buf_pos;
        if (avail == 0) {
            r->read_buf_pos = 0;
            r->read_buf_len = 0;
            int64_t n = r->recv_fn(r->ctx, r->read_buf, r->read_buf_cap);
            if (n <= 0) {
                return -1;
            }
            r->read_buf_len = (size_t)n;
            avail           = (size_t)n;
        }

        char* src = r->read_buf + r->read_buf_pos;
        for (size_t i = 0; i < avail && pos < len; i++) {
            dst[pos++] = src[i];
            r->read_buf_pos++;

            if (pos >= delim_len
                && memcmp(dst + pos - delim_len, delim, delim_len) == 0) {
                pos -= delim_len;
                dst[pos] = '\0';
                return (int64_t)pos;
            }
        }
    }

    xylem_loge("fd=%d recv: delimiter not found within buffer", r->fd);
    return -1;
}

int64_t framing_recv(framing_t*           r,
                     const xylem_framing_opts_t* opts,
                     void*                       buf,
                     size_t                      len) {
    if (opts->type == XYLEM_FRAMING_NONE) {
        return r->recv_fn(r->ctx, buf, len);
    }

    if (!r->read_buf) {
        r->read_buf = (char*)malloc(r->read_buf_cap);
        if (!r->read_buf) {
            return -1;
        }
    }

    switch (opts->type) {
    case XYLEM_FRAMING_FIXED:
        return _framing_recv_fixed(r, opts, buf, len);
    case XYLEM_FRAMING_LENFIELD_FIXINT:
        return _framing_recv_lenfield_fixint(r, opts, buf, len);
    case XYLEM_FRAMING_LENFIELD_VARINT:
        return _framing_recv_lenfield_varint(r, opts, buf, len);
    case XYLEM_FRAMING_DELIMITER:
        return _framing_recv_delimiter(r, opts, buf, len);
    default:
        return -1;
    }
}

static int
_framing_send_lenfield_fixint(framing_t* r, const xylem_framing_opts_t* opts,
                              framing_send_fn send_fn, const void* data,
                              size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = opts->lenfield_fixint.header_size;

    if (hdr_sz > sizeof(hdr)) {
        xylem_loge("fd=%d send: header_size %u exceeds limit",
                   r->fd, hdr_sz);
        return -1;
    }

    int64_t wire_len = (int64_t)len - opts->lenfield_fixint.adjustment;
    if (wire_len < 0) {
        xylem_loge("fd=%d send: negative wire length", r->fd);
        return -1;
    }

    memset(hdr, 0, hdr_sz);

    uint8_t* field = hdr + opts->lenfield_fixint.field_offset;
    uint64_t val   = (uint64_t)wire_len;

    if (opts->lenfield_fixint.big_endian) {
        for (int32_t i = (int32_t)opts->lenfield_fixint.field_size - 1;
             i >= 0; i--) {
            field[i] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
    } else {
        for (uint32_t i = 0; i < opts->lenfield_fixint.field_size; i++) {
            field[i] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
    }

    if (send_fn(r->ctx, hdr, hdr_sz) != 0) {
        return -1;
    }
    return send_fn(r->ctx, data, len);
}

static int
_framing_send_lenfield_varint(framing_t* r, const xylem_framing_opts_t* opts,
                              framing_send_fn send_fn, const void* data,
                              size_t len) {
    int64_t wire_len = (int64_t)len - opts->lenfield_varint.adjustment;
    if (wire_len < 0) {
        xylem_loge("fd=%d send: negative wire length", r->fd);
        return -1;
    }

    uint8_t hdr[10];
    size_t  pos = 0;
    if (!xylem_varint_encode((uint64_t)wire_len, hdr, sizeof(hdr), &pos)) {
        xylem_loge("fd=%d send: varint encode failed", r->fd);
        return -1;
    }
    if (send_fn(r->ctx, hdr, pos) != 0) {
        return -1;
    }
    return send_fn(r->ctx, data, len);
}

static int
_framing_send_fixed(framing_t* r, const xylem_framing_opts_t* opts,
            framing_send_fn send_fn, const void* data, size_t len) {
    if (len != opts->fixed.len) {
        xylem_loge("fd=%d send: payload %zu != fixed frame %zu",
                   r->fd, len, opts->fixed.len);
        return -1;
    }
    return send_fn(r->ctx, data, len);
}

static int
_framing_send_delimiter(framing_t* r, const xylem_framing_opts_t* opts,
                framing_send_fn send_fn, const void* data, size_t len) {
    if (send_fn(r->ctx, data, len) != 0) {
        return -1;
    }
    size_t delim_len = opts->delimiter.delim_len;
    if (delim_len == 0) {
        delim_len = strlen(opts->delimiter.delim);
    }
    return send_fn(r->ctx, opts->delimiter.delim, delim_len);
}

int framing_send(framing_t*                  f,
                 const xylem_framing_opts_t* opts,
                 const void*                 data,
                 size_t                      len) {
    switch (opts->type) {
    case XYLEM_FRAMING_FIXED:
        return _framing_send_fixed(f, opts, f->send_fn, data, len);
    case XYLEM_FRAMING_LENFIELD_FIXINT:
        return _framing_send_lenfield_fixint(f, opts, f->send_fn, data, len);
    case XYLEM_FRAMING_LENFIELD_VARINT:
        return _framing_send_lenfield_varint(f, opts, f->send_fn, data, len);
    case XYLEM_FRAMING_DELIMITER:
        return _framing_send_delimiter(f, opts, f->send_fn, data, len);
    default:
        return f->send_fn(f->ctx, data, len);
    }
}

void framing_init(
    framing_t* f, framing_recv_fn recv_fn, framing_send_fn send_fn,
    void* ctx, int fd, size_t buf_cap) {
    f->recv_fn      = recv_fn;
    f->send_fn      = send_fn;
    f->ctx          = ctx;
    f->fd           = fd;
    f->read_buf     = NULL;
    f->read_buf_cap = buf_cap;
    f->read_buf_pos = 0;
    f->read_buf_len = 0;
}

void framing_deinit(framing_t* f) {
    free(f->read_buf);
    f->read_buf = NULL;
}
