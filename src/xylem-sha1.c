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
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buffer[64];
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
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    return ctx;
}

void xylem_sha1_update(xylem_sha1_t* ctx, const uint8_t* data, size_t len) {
    uint32_t i, j;
    j = ctx->count[0];
    if ((ctx->count[0] += ((uint32_t)len << 3)) < j) {
        ctx->count[1]++;
    }
    ctx->count[1] += (uint32_t)(len >> 29);
    j = (j >> 3) & 63;

    if ((j + len) > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        _sha1_transform(ctx->state, ctx->buffer);
        for (; i + 64 <= len; i += 64) {
            _sha1_transform(ctx->state, &data[i]);
        }
        j = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

void xylem_sha1_final(xylem_sha1_t* ctx, uint8_t digest[20]) {
    uint64_t bitlen = ((uint64_t)ctx->count[1] << 32) | ctx->count[0];
    uint32_t bytelen = (uint32_t)(bitlen >> 3);
    uint32_t pos = bytelen & 63;

    ctx->buffer[pos++] = 0x80;

    if (pos > 55) {
        while (pos < 64) {
            ctx->buffer[pos++] = 0;
        }
        _sha1_transform(ctx->state, ctx->buffer);
        pos = 0;
    }
    while (pos < 56) {
        ctx->buffer[pos++] = 0;
    }

    ctx->buffer[56] = (uint8_t)((bitlen >> 56) & 0xFF);
    ctx->buffer[57] = (uint8_t)((bitlen >> 48) & 0xFF);
    ctx->buffer[58] = (uint8_t)((bitlen >> 40) & 0xFF);
    ctx->buffer[59] = (uint8_t)((bitlen >> 32) & 0xFF);
    ctx->buffer[60] = (uint8_t)((bitlen >> 24) & 0xFF);
    ctx->buffer[61] = (uint8_t)((bitlen >> 16) & 0xFF);
    ctx->buffer[62] = (uint8_t)((bitlen >> 8) & 0xFF);
    ctx->buffer[63] = (uint8_t)((bitlen)&0xFF);

    _sha1_transform(ctx->state, ctx->buffer);

    for (unsigned i = 0; i < 20; ++i) {
        digest[i] =
            (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
    }
}

void xylem_sha1_destroy(xylem_sha1_t* ctx) {
    free(ctx);
}
