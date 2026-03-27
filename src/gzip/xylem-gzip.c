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

#include "xylem/xylem-gzip.h"

#include <string.h>
#include "miniz.h"

#define GZIP_HEADER_SIZE 10
#define GZIP_TRAILER_SIZE 8
#define GZIP_OVERHEAD (GZIP_HEADER_SIZE + GZIP_TRAILER_SIZE)

static uint32_t _gzip_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void _gzip_write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/**
 * Skip past the gzip header fields (RFC 1952) and return a pointer to the
 * start of the compressed payload. Returns NULL if the header is malformed.
 * Sets *hdr_len to the total number of header bytes consumed.
 */
static const uint8_t *_gzip_skip_header(const uint8_t *src, size_t src_len,
                                        size_t *hdr_len) {
    if (src_len < GZIP_OVERHEAD) {
        return NULL;
    }
    if (src[0] != 0x1f || src[1] != 0x8b) {
        return NULL;
    }
    if (src[2] != 0x08) {
        return NULL;
    }

    uint8_t flags = src[3];
    size_t pos = GZIP_HEADER_SIZE;

    /* FEXTRA */
    if (flags & 0x04) {
        if (pos + 2 > src_len) {
            return NULL;
        }
        uint16_t xlen = (uint16_t)src[pos] | ((uint16_t)src[pos + 1] << 8);
        pos += 2 + xlen;
        if (pos > src_len) {
            return NULL;
        }
    }
    /* FNAME */
    if (flags & 0x08) {
        while (pos < src_len && src[pos] != 0) {
            pos++;
        }
        if (pos >= src_len) {
            return NULL;
        }
        pos++;
    }
    /* FCOMMENT */
    if (flags & 0x10) {
        while (pos < src_len && src[pos] != 0) {
            pos++;
        }
        if (pos >= src_len) {
            return NULL;
        }
        pos++;
    }
    /* FHCRC */
    if (flags & 0x02) {
        pos += 2;
        if (pos > src_len) {
            return NULL;
        }
    }

    *hdr_len = pos;
    return src + pos;
}

/* Raw deflate into caller-provided buffer. Returns bytes written or -1. */
static int _gzip_raw_deflate(const uint8_t *src, size_t slen, uint8_t *dst,
                             size_t dlen, int level) {
    if (slen > UINT_MAX || dlen > UINT_MAX) {
        return -1;
    }

    mz_stream stream;
    memset(&stream, 0, sizeof(stream));

    int rc = mz_deflateInit2(&stream, level, MZ_DEFLATED,
                             -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY);
    if (rc != MZ_OK) {
        return -1;
    }

    stream.next_in = (const unsigned char *)src;
    stream.avail_in = (unsigned int)slen;
    stream.next_out = (unsigned char *)dst;
    stream.avail_out = (unsigned int)dlen;

    rc = mz_deflate(&stream, MZ_FINISH);
    mz_deflateEnd(&stream);

    if (rc != MZ_STREAM_END) {
        return -1;
    }

    return (int)stream.total_out;
}

/* Raw inflate into caller-provided buffer. Returns bytes written or -1. */
static int _gzip_raw_inflate(const uint8_t *src, size_t slen, uint8_t *dst,
                             size_t dlen) {
    if (slen > UINT_MAX || dlen > UINT_MAX) {
        return -1;
    }

    mz_stream stream;
    memset(&stream, 0, sizeof(stream));

    int rc = mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS);
    if (rc != MZ_OK) {
        return -1;
    }

    stream.next_in = (const unsigned char *)src;
    stream.avail_in = (unsigned int)slen;
    stream.next_out = (unsigned char *)dst;
    stream.avail_out = (unsigned int)dlen;

    rc = mz_inflate(&stream, MZ_FINISH);
    mz_inflateEnd(&stream);

    if (rc != MZ_STREAM_END) {
        return -1;
    }

    return (int)stream.total_out;
}

size_t xylem_gzip_deflate_bound(size_t slen) {
    mz_stream stream;
    memset(&stream, 0, sizeof(stream));
    mz_deflateInit2(&stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED,
                    -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY);
    mz_ulong bound = mz_deflateBound(&stream, (mz_ulong)slen);
    mz_deflateEnd(&stream);
    return (size_t)bound;
}

size_t xylem_gzip_compress_bound(size_t slen) {
    return xylem_gzip_deflate_bound(slen) + GZIP_OVERHEAD;
}

int xylem_gzip_compress(const uint8_t *src, size_t slen, uint8_t *dst,
                        size_t dlen, int level) {
    if (!dst || dlen < GZIP_OVERHEAD) {
        return -1;
    }
    if (slen > 0 && !src) {
        return -1;
    }

    /* Write 10-byte gzip header. */
    dst[0] = 0x1f;
    dst[1] = 0x8b;
    dst[2] = 0x08;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;
    dst[6] = 0x00;
    dst[7] = 0x00;
    dst[8] = 0x00;
    dst[9] = 0xff;

    int deflated = _gzip_raw_deflate(src, slen, dst + GZIP_HEADER_SIZE,
                                     dlen - GZIP_OVERHEAD, level);
    if (deflated < 0) {
        return -1;
    }

    /* 8-byte trailer: CRC-32 + original size (mod 2^32). */
    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, (const unsigned char *)src, slen);
    uint8_t *trailer = dst + GZIP_HEADER_SIZE + deflated;
    _gzip_write_le32(trailer, (uint32_t)crc);
    _gzip_write_le32(trailer + 4, (uint32_t)(slen & 0xFFFFFFFF));

    return GZIP_HEADER_SIZE + deflated + GZIP_TRAILER_SIZE;
}

int xylem_gzip_decompress(const uint8_t *src, size_t slen, uint8_t *dst,
                          size_t dlen) {
    if (!src || !dst) {
        return -1;
    }

    size_t hdr_len = 0;
    const uint8_t *payload = _gzip_skip_header(src, slen, &hdr_len);
    if (!payload) {
        return -1;
    }

    if (slen < hdr_len + GZIP_TRAILER_SIZE) {
        return -1;
    }
    size_t payload_len = slen - hdr_len - GZIP_TRAILER_SIZE;
    const uint8_t *trailer = src + slen - GZIP_TRAILER_SIZE;
    uint32_t expected_crc = _gzip_read_le32(trailer);
    uint32_t expected_size = _gzip_read_le32(trailer + 4);

    int written = _gzip_raw_inflate(payload, payload_len, dst, dlen);
    if (written < 0) {
        return -1;
    }

    /* Verify CRC-32 and original size. */
    mz_ulong crc =
        mz_crc32(MZ_CRC32_INIT, (const unsigned char *)dst, (size_t)written);
    if ((uint32_t)crc != expected_crc ||
        (uint32_t)((size_t)written & 0xFFFFFFFF) != expected_size) {
        return -1;
    }

    return written;
}

int xylem_gzip_deflate(const uint8_t *src, size_t slen, uint8_t *dst,
                       size_t dlen, int level) {
    if (!dst) {
        return -1;
    }
    if (slen > 0 && !src) {
        return -1;
    }
    return _gzip_raw_deflate(src, slen, dst, dlen, level);
}

int xylem_gzip_inflate(const uint8_t *src, size_t slen, uint8_t *dst,
                       size_t dlen) {
    if (!src || !dst) {
        return -1;
    }
    return _gzip_raw_inflate(src, slen, dst, dlen);
}
