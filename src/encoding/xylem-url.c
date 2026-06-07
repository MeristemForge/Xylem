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

#include "xylem/encoding/xylem-url.h"

#include <limits.h>

static const char _hex[] = "0123456789ABCDEF";

/* Values are actual hex value + 1; 0 means invalid. */
static const uint8_t _hex_table[256] = {
    ['0'] = 1,  ['1'] = 2,  ['2'] = 3,  ['3'] = 4,
    ['4'] = 5,  ['5'] = 6,  ['6'] = 7,  ['7'] = 8,
    ['8'] = 9,  ['9'] = 10,
    ['A'] = 11, ['B'] = 12, ['C'] = 13, ['D'] = 14, ['E'] = 15, ['F'] = 16,
    ['a'] = 11, ['b'] = 12, ['c'] = 13, ['d'] = 14, ['e'] = 15, ['f'] = 16,
};

/* RFC 3986 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~" */
static const uint8_t _unreserved[256] = {
    ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1,
    ['G'] = 1, ['H'] = 1, ['I'] = 1, ['J'] = 1, ['K'] = 1, ['L'] = 1,
    ['M'] = 1, ['N'] = 1, ['O'] = 1, ['P'] = 1, ['Q'] = 1, ['R'] = 1,
    ['S'] = 1, ['T'] = 1, ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1,
    ['Y'] = 1, ['Z'] = 1,
    ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1,
    ['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1, ['l'] = 1,
    ['m'] = 1, ['n'] = 1, ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1,
    ['s'] = 1, ['t'] = 1, ['u'] = 1, ['v'] = 1, ['w'] = 1, ['x'] = 1,
    ['y'] = 1, ['z'] = 1,
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1,
    ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
    ['-'] = 1, ['.'] = 1, ['_'] = 1, ['~'] = 1,
};

int xylem_url_encode_size(int slen) {
    if (slen < 0 || slen > INT_MAX / 3) return -1;
    return slen * 3;
}

int xylem_url_decode_size(int slen) {
    return slen;
}

int xylem_url_encode(const uint8_t* src, int slen, uint8_t* dst, int dlen) {
    if (!src || !dst || slen < 0 || slen > INT_MAX / 3 || dlen < slen * 3) {
        return -1;
    }
    int j = 0;
    for (int i = 0; i < slen; i++) {
        uint8_t c = src[i];
        if (_unreserved[c]) {
            dst[j++] = c;
        } else {
            dst[j++] = '%';
            dst[j++] = (uint8_t)_hex[c >> 4];
            dst[j++] = (uint8_t)_hex[c & 0x0F];
        }
    }
    return j;
}

int xylem_url_decode(const uint8_t* src, int slen, uint8_t* dst, int dlen) {
    if (!src || !dst || slen < 0 || dlen < slen) {
        return -1;
    }
    int j = 0;
    for (int i = 0; i < slen; i++) {
        if (src[i] == '%' && i + 2 < slen) {
            uint8_t hi = _hex_table[src[i + 1]];
            uint8_t lo = _hex_table[src[i + 2]];
            if (hi && lo) {
                dst[j++] = (uint8_t)(((hi - 1) << 4) | (lo - 1));
                i += 2;
                continue;
            }
        }
        dst[j++] = src[i];
    }
    return j;
}
