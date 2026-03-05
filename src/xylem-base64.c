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

static const char b64_enc_std[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const char b64_enc_url[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static const int8_t b64_dec_std[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60,
    61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1,
    -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
    43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1};

static const int8_t b64_dec_url[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, 52, 53, 54, 55, 56, 57, 58, 59, 60,
    61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1,
    63, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
    43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1};

static int _base64_encode(
    const uint8_t* src,
    int            slen,
    uint8_t*       dst,
    int            dlen,
    const char*    table,
    bool           padding) {
    if (dlen < ((slen + 2) / 3) * 4) {
        return -1;
    }
    uint8_t* tmp = dst;
    uint32_t val = 0;

    while (slen >= 3) {
        val = (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2];

        *tmp++ = table[val >> 18];
        *tmp++ = table[(val >> 12) & 0x3f];
        *tmp++ = table[(val >> 6) & 0x3f];
        *tmp++ = table[val & 0x3f];

        src += 3;
        slen -= 3;
    }

    switch (slen) {
    case 2:
        val = (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8;

        *tmp++ = table[val >> 18];
        *tmp++ = table[(val >> 12) & 0x3f];
        *tmp++ = table[(val >> 6) & 0x3f];

        if (padding) {
            *tmp++ = '=';
        }
        break;
    case 1:
        val = (uint32_t)src[0] << 16;

        *tmp++ = table[val >> 18];
        *tmp++ = table[(val >> 12) & 0x3f];

        if (padding) {
            *tmp++ = '=';
            *tmp++ = '=';
        }
        break;
    }
    return tmp - dst;
}

static int _base64_decode(
    const uint8_t* src,
    int            slen,
    uint8_t*       dst,
    int            dlen,
    const int8_t*  table,
    bool           padding) {
    if (dlen < ((slen + 3) / 4) * 3) {
        return -1;
    }
    uint8_t* tmp = dst;
    int32_t  val = 0;
    int8_t   input[4] = {0};

    while (slen >= 4) {
        input[0] = table[src[0]];
        input[1] = table[src[1]];
        input[2] = table[src[2]];
        input[3] = table[src[3]];

        val = input[0] << 18 | input[1] << 12 | input[2] << 6 | input[3];

        if (val < 0) {
            if (!padding || slen != 4 || src[3] != '=') {
                return -1;
            }
            padding = 0;
            slen = src[2] == '=' ? 2 : 3;
            break;
        }

        *tmp++ = val >> 16;
        *tmp++ = val >> 8;
        *tmp++ = val;

        src += 4;
        slen -= 4;
    }

    if (!slen) {
        return tmp - dst;
    }
    if (padding || slen == 1) {
        return -1;
    }

    int8_t v0 = table[src[0]];
    int8_t v1 = table[src[1]];
    if (v0 < 0 || v1 < 0) {
        return -1;
    }

    if (slen == 2) {
        val = (v0 << 12) | (v1 << 6);
        if (val & 0x3ff) {
            return -1;
        }
        *tmp++ = val >> 10;
    } else {
        int8_t v2 = table[src[2]];
        if (v2 < 0) {
            return -1;
        }
        val = (v0 << 12) | (v1 << 6) | v2;
        if (val & 0x3) {
            return -1;
        }
        *tmp++ = val >> 10;
        *tmp++ = val >> 2;
    }
    return tmp - dst;
}

int xylem_base64_encode_std(
    const uint8_t* src, int slen, uint8_t* dst, int dlen) {
    return _base64_encode(src, slen, dst, dlen, b64_enc_std, true);
}

int xylem_base64_decode_std(
    const uint8_t* src, int slen, uint8_t* dst, int dlen) {
    return _base64_decode(src, slen, dst, dlen, b64_dec_std, true);
}

int xylem_base64_encode_url(
    const uint8_t* src, int slen, uint8_t* dst, int dlen, bool padding) {
    return _base64_encode(src, slen, dst, dlen, b64_enc_url, padding);
}

int xylem_base64_decode_url(
    const uint8_t* src, int slen, uint8_t* dst, int dlen, bool padding) {
    return _base64_decode(src, slen, dst, dlen, b64_dec_url, padding);
}