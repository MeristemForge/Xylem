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

struct xylem_ringbuf_s {
    char*    buf;
    uint64_t wpos; /* write pos  */
    uint64_t rpos; /* read pos   */
    uint32_t mask; /* mask = cap - 1 */
    uint32_t esz;  /* entry size (bytes) */
};

static inline uint32_t _ringbuffer_rounddown_pow_of_two(uint32_t n) {
    if (n == 0) {
        return 0;
    }
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return (n + 1) >> 1;
}

static inline void _ringbuffer_internal_write(
    xylem_ringbuf_t* ring, const void* src, uint32_t len, uint64_t off) {

    uint32_t idx = (uint32_t)(off & ring->mask);
    uint32_t cap = ring->mask + 1;
    uint32_t esize = ring->esz;

    if (esize == 1) {
        uint32_t l = (len <= cap - idx) ? len : (cap - idx);
        memcpy(ring->buf + idx, src, l);
        if (l < len) {
            memcpy(ring->buf, (const char*)src + l, len - l);
        }
    } else {
        size_t byte_off = (size_t)idx * (size_t)esize;
        size_t total_bytes = (size_t)len * (size_t)esize;
        size_t buf_size = (size_t)cap * (size_t)esize;
        size_t l = (total_bytes <= buf_size - byte_off) ? total_bytes
                                                        : (buf_size - byte_off);

        memcpy(ring->buf + byte_off, src, l);
        if (l < total_bytes) {
            memcpy(ring->buf, (const char*)src + l, total_bytes - l);
        }
    }
}

static inline void _ringbuffer_internal_read(
    xylem_ringbuf_t* ring, void* dst, uint32_t len, uint64_t off) {

    uint32_t idx = (uint32_t)(off & ring->mask);
    uint32_t cap = ring->mask + 1;
    uint32_t esize = ring->esz;

    if (esize == 1) {
        uint32_t l = (len <= cap - idx) ? len : (cap - idx);
        memcpy(dst, ring->buf + idx, l);
        if (l < len) {
            memcpy((char*)dst + l, ring->buf, len - l);
        }
    } else {
        size_t byte_off = (size_t)idx * (size_t)esize;
        size_t total_bytes = (size_t)len * (size_t)esize;
        size_t buf_size = (size_t)cap * (size_t)esize;
        size_t l = (total_bytes <= buf_size - byte_off) ? total_bytes
                                                        : (buf_size - byte_off);

        memcpy(dst, ring->buf + byte_off, l);
        if (l < total_bytes) {
            memcpy((char*)dst + l, ring->buf, total_bytes - l);
        }
    }
}

static inline uint32_t
_ringbuffer_internal_read_peek(xylem_ringbuf_t* ring, void* buf, uint32_t len) {
    uint64_t current_len = ring->wpos - ring->rpos;
    if (current_len > UINT32_MAX) {
        current_len = UINT32_MAX;
    }
    uint32_t actual_len = (uint32_t)current_len;
    if (len > actual_len) {
        len = actual_len;
    }
    _ringbuffer_internal_read(ring, buf, len, ring->rpos);
    return len;
}

xylem_ringbuf_t* xylem_ringbuf_create(size_t esize, size_t bufsize) {
    if (esize == 0 || bufsize < esize) {
        return NULL;
    }
    if (esize > UINT32_MAX) {
        return NULL;
    }
    size_t elem_count = bufsize / esize;
    if (elem_count == 0) {
        return NULL;
    }
    if (elem_count > UINT32_MAX) {
        elem_count = UINT32_MAX;
    }
    uint32_t n = (uint32_t)elem_count;
    uint32_t cap = _ringbuffer_rounddown_pow_of_two(n);
    if (cap == 0) {
        return NULL;
    }
    size_t actual_buf_size = (size_t)cap * esize;

    xylem_ringbuf_t* ring = (xylem_ringbuf_t*)malloc(sizeof(xylem_ringbuf_t));
    if (!ring) {
        return NULL;
    }
    ring->buf = (char*)malloc(actual_buf_size);
    if (!ring->buf) {
        free(ring);
        return NULL;
    }
    ring->esz = (uint32_t)esize;
    ring->mask = cap - 1;
    ring->wpos = 0;
    ring->rpos = 0;

    return ring;
}

void xylem_ringbuf_destroy(xylem_ringbuf_t* ring) {
    free(ring->buf);
    free(ring);
}

bool xylem_ringbuf_full(xylem_ringbuf_t* ring) {
    return xylem_ringbuf_len(ring) >= xylem_ringbuf_cap(ring);
}

bool xylem_ringbuf_empty(xylem_ringbuf_t* ring) {
    return ring->wpos == ring->rpos;
}

size_t xylem_ringbuf_len(xylem_ringbuf_t* ring) {
    return (size_t)(ring->wpos - ring->rpos);
}

size_t xylem_ringbuf_cap(xylem_ringbuf_t* ring) {
    return (size_t)(ring->mask + 1);
}

size_t xylem_ringbuf_avail(xylem_ringbuf_t* ring) {
    size_t len = xylem_ringbuf_len(ring);
    size_t cap = xylem_ringbuf_cap(ring);
    return (len >= cap) ? 0 : (cap - len);
}

size_t xylem_ringbuf_write(
    xylem_ringbuf_t* ring, const void* buf, size_t entry_count) {

    size_t avail = xylem_ringbuf_avail(ring);
    if (entry_count > avail) {
        entry_count = avail;
    }

    _ringbuffer_internal_write(ring, buf, (uint32_t)entry_count, ring->wpos);
    ring->wpos += (uint32_t)entry_count;

    return entry_count;
}

size_t
xylem_ringbuf_read(xylem_ringbuf_t* ring, void* buf, size_t entry_count) {
    size_t len = xylem_ringbuf_len(ring);
    if (entry_count > len) {
        entry_count = len;
    }
    uint32_t count32 = (uint32_t)entry_count;

    _ringbuffer_internal_read(ring, buf, count32, ring->rpos);
    ring->rpos += count32;

    return (size_t)count32;
}