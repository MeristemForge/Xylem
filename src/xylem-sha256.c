/**
 *  Filename:   sha256.c
 *  Author:     Brad Conte (brad AT bradconte.com)
 *  Copyright:
 *  Disclaimer: This code is presented "as is" without any guarantees.
 *  Details:    Implementation of the SHA-256 hashing algorithm.
 *              SHA-256 is one of the three algorithms in the SHA2
 *              specification. The others, SHA-384 and SHA-512, are not
 *              offered in this implementation.
 *              Algorithm specification can be found here:
 *              *
 * http://csrc.nist.gov/publications/fips/fips180-2/fips180-2withchangenotice.pdf
 *              This implementation uses little endian byte order.
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

#define ROTLEFT(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define ROTRIGHT(a, b) (((a) >> (b)) | ((a) << (32 - (b))))

#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x, 2) ^ ROTRIGHT(x, 13) ^ ROTRIGHT(x, 22))
#define EP1(x) (ROTRIGHT(x, 6) ^ ROTRIGHT(x, 11) ^ ROTRIGHT(x, 25))
#define SIG0(x) (ROTRIGHT(x, 7) ^ ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x, 17) ^ ROTRIGHT(x, 19) ^ ((x) >> 10))

struct xylem_sha256_s {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
};

static const uint32_t _k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline void
_sha256_transform(xylem_sha256_t* ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int      i, j;

    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j + 0] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + _k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

xylem_sha256_t* xylem_sha256_create(void) {
    xylem_sha256_t* ctx = malloc(sizeof(xylem_sha256_t));
    if (!ctx) {
        return NULL;
    }
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    memset(ctx->data, 0, 64);
    return ctx;
}

void xylem_sha256_update(xylem_sha256_t* ctx, const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t space = 64 - ctx->datalen;
        size_t to_copy = (len - i < space) ? len - i : space;
        memcpy(ctx->data + ctx->datalen, data + i, to_copy);
        ctx->datalen += (uint32_t)to_copy;
        i += to_copy;

        if (ctx->datalen == 64) {
            _sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void xylem_sha256_final(xylem_sha256_t* ctx, uint8_t digest[32]) {
    uint32_t orig_datalen = ctx->datalen;
    uint64_t total_bitlen = ctx->bitlen + ((uint64_t)orig_datalen * 8);

    ctx->data[orig_datalen] = 0x80;
    ctx->datalen = orig_datalen + 1;

    if (orig_datalen < 56) {
        memset(ctx->data + ctx->datalen, 0, 56 - ctx->datalen);
        ctx->datalen = 56;
    } else {
        memset(ctx->data + ctx->datalen, 0, 64 - ctx->datalen);
        ctx->datalen = 64;
        _sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
        ctx->datalen = 56;
    }
    ctx->data[56] = (uint8_t)(total_bitlen >> 56);
    ctx->data[57] = (uint8_t)(total_bitlen >> 48);
    ctx->data[58] = (uint8_t)(total_bitlen >> 40);
    ctx->data[59] = (uint8_t)(total_bitlen >> 32);
    ctx->data[60] = (uint8_t)(total_bitlen >> 24);
    ctx->data[61] = (uint8_t)(total_bitlen >> 16);
    ctx->data[62] = (uint8_t)(total_bitlen >> 8);
    ctx->data[63] = (uint8_t)(total_bitlen);

    _sha256_transform(ctx, ctx->data);

    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = (ctx->state[i] >> 24) & 0xFF;
        digest[i * 4 + 1] = (ctx->state[i] >> 16) & 0xFF;
        digest[i * 4 + 2] = (ctx->state[i] >> 8) & 0xFF;
        digest[i * 4 + 3] = ctx->state[i] & 0xFF;
    }
}

void xylem_sha256_destroy(xylem_sha256_t* ctx) {
    memset(ctx, 0, sizeof(xylem_sha256_t));
    free(ctx);
}
