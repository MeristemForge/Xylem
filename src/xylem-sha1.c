/**
 *  SHA-1 in C
 *  By Steve Reid <steve@edmweb.com>
 *  100% Public Domain
 *
 *  Modified 01/2026
 *  By Jin.Wu <wujin.developer@gmail.com>
 *  Still 100% PD for original parts.
 *
 *  Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
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

struct xylem_sha1_s {
    uint8_t  buffer[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[5];
};

static inline uint32_t _sha1_rol32(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

static inline void
_sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t W[80];
    uint32_t a, b, c, d, e;
    uint32_t temp;

    for (int i = 0; i < 16; ++i) {
        W[i] = ((uint32_t)buffer[i * 4 + 0] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        W[i] = _sha1_rol32(W[i - 3] ^ W[i - 8] ^ W[i - 14] ^ W[i - 16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (int i = 0; i < 20; ++i) {
        temp =
            _sha1_rol32(a, 5) + ((b & c) | ((~b) & d)) + e + W[i] + 0x5A827999;
        e = d;
        d = c;
        c = _sha1_rol32(b, 30);
        b = a;
        a = temp;
    }
    for (int i = 20; i < 40; ++i) {
        temp = _sha1_rol32(a, 5) + (b ^ c ^ d) + e + W[i] + 0x6ED9EBA1;
        e = d;
        d = c;
        c = _sha1_rol32(b, 30);
        b = a;
        a = temp;
    }
    for (int i = 40; i < 60; ++i) {
        temp = _sha1_rol32(a, 5) + ((b & c) | (b & d) | (c & d)) + e + W[i] +
               0x8F1BBCDC;
        e = d;
        d = c;
        c = _sha1_rol32(b, 30);
        b = a;
        a = temp;
    }
    for (int i = 60; i < 80; ++i) {
        temp = _sha1_rol32(a, 5) + (b ^ c ^ d) + e + W[i] + 0xCA62C1D6;
        e = d;
        d = c;
        c = _sha1_rol32(b, 30);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

xylem_sha1_t* xylem_sha1_create(void) {
    xylem_sha1_t* ctx = (xylem_sha1_t*)malloc(sizeof(xylem_sha1_t));
    if (!ctx) {
        return NULL;
    }
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->datalen = 0;
    ctx->bitlen = 0;
    return ctx;
}

void xylem_sha1_update(xylem_sha1_t* ctx, const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t space = 64 - ctx->datalen;
        size_t to_copy = (len - i < space) ? len - i : space;
        memcpy(ctx->buffer + ctx->datalen, data + i, to_copy);
        ctx->datalen += (uint32_t)to_copy;
        i += to_copy;

        if (ctx->datalen == 64) {
            _sha1_transform(ctx->state, ctx->buffer);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void xylem_sha1_final(xylem_sha1_t* ctx, uint8_t digest[20]) {
    uint32_t orig_datalen = ctx->datalen;
    uint64_t total_bitlen = ctx->bitlen + ((uint64_t)orig_datalen * 8);

    ctx->buffer[orig_datalen] = 0x80;
    ctx->datalen = orig_datalen + 1;

    if (orig_datalen < 56) {
        memset(ctx->buffer + ctx->datalen, 0, 56 - ctx->datalen);
        ctx->datalen = 56;
    } else {
        memset(ctx->buffer + ctx->datalen, 0, 64 - ctx->datalen);
        ctx->datalen = 64;
        _sha1_transform(ctx->state, ctx->buffer);
        memset(ctx->buffer, 0, 56);
        ctx->datalen = 56;
    }

    ctx->buffer[56] = (uint8_t)(total_bitlen >> 56);
    ctx->buffer[57] = (uint8_t)(total_bitlen >> 48);
    ctx->buffer[58] = (uint8_t)(total_bitlen >> 40);
    ctx->buffer[59] = (uint8_t)(total_bitlen >> 32);
    ctx->buffer[60] = (uint8_t)(total_bitlen >> 24);
    ctx->buffer[61] = (uint8_t)(total_bitlen >> 16);
    ctx->buffer[62] = (uint8_t)(total_bitlen >> 8);
    ctx->buffer[63] = (uint8_t)(total_bitlen);

    _sha1_transform(ctx->state, ctx->buffer);

    for (unsigned i = 0; i < 20; ++i) {
        digest[i] =
            (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
    }
}

void xylem_sha1_destroy(xylem_sha1_t* ctx) {
    memset(ctx, 0, sizeof(xylem_sha1_t));
    free(ctx);
}
