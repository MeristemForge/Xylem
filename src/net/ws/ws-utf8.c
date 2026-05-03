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

#include "ws-utf8.h"

int ws_utf8_validate(const uint8_t* data, size_t len) {
    size_t i = 0;

    while (i < len) {
        uint8_t b = data[i];

        if (b <= 0x7F) {
            /* Fast-skip consecutive ASCII bytes */
            i++;
            while (i < len && data[i] <= 0x7F) {
                i++;
            }
        } else if ((b & 0xE0) == 0xC0) {
            /* Two-byte sequence (U+0080 - U+07FF) */
            if (b < 0xC2) {
                /* Overlong: 0xC0 and 0xC1 lead bytes encode < U+0080 */
                return -1;
            }
            if (i + 1 >= len) {
                return -1;
            }
            if ((data[i + 1] & 0xC0) != 0x80) {
                return -1;
            }
            i += 2;
        } else if ((b & 0xF0) == 0xE0) {
            /* Three-byte sequence (U+0800 - U+FFFF) */
            if (i + 2 >= len) {
                return -1;
            }
            uint8_t b1 = data[i + 1];
            uint8_t b2 = data[i + 2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
                return -1;
            }
            /* Reject overlong: E0 must be followed by A0-BF */
            if (b == 0xE0 && b1 < 0xA0) {
                return -1;
            }
            /* Reject surrogates: U+D800-U+DFFF (ED A0-BF xx) */
            if (b == 0xED && b1 >= 0xA0) {
                return -1;
            }
            i += 3;
        } else if ((b & 0xF8) == 0xF0) {
            /* Four-byte sequence (U+10000 - U+10FFFF) */
            if (b > 0xF4) {
                /* Lead byte F5-F7 would encode > U+10FFFF */
                return -1;
            }
            if (i + 3 >= len) {
                return -1;
            }
            uint8_t b1 = data[i + 1];
            uint8_t b2 = data[i + 2];
            uint8_t b3 = data[i + 3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 ||
                (b3 & 0xC0) != 0x80) {
                return -1;
            }
            /* Reject overlong: F0 must be followed by 90-BF */
            if (b == 0xF0 && b1 < 0x90) {
                return -1;
            }
            /* Reject > U+10FFFF: F4 must be followed by 80-8F */
            if (b == 0xF4 && b1 > 0x8F) {
                return -1;
            }
            i += 4;
        } else {
            /* Invalid lead byte: continuation bytes (80-BF), F5-FF */
            return -1;
        }
    }

    return 0;
}
