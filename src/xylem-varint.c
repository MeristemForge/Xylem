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

#include "xylem/xylem-varint.h"

bool xylem_varint_encode(
    uint64_t value, uint8_t* buf, size_t bufsize, size_t* pos) {
    if (!buf || bufsize == 0) {
        return false;
    }
    size_t write_pos = pos ? *pos : 0;

    if (write_pos > bufsize) {
        return false;
    }
    while (value > 0x7F) {
        if (write_pos >= bufsize) {
            return false;
        }
        buf[write_pos++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    if (write_pos >= bufsize) {
        return false;
    }
    buf[write_pos++] = (uint8_t)value;

    if (pos) {
        *pos = write_pos;
    }
    return true;
}

bool xylem_varint_decode(
    const uint8_t* buf, size_t bufsize, size_t* pos, uint64_t* out_value) {
    if (!buf || !pos) {
        return false;
    }
    size_t p = *pos;
    if (p >= bufsize) {
        return false;
    }
    uint64_t value = 0;
    int      shift = 0;

    while (1) {
        if (p >= bufsize) {
            return false;
        }
        uint8_t byte = buf[p++];
        if (shift == 63 && (byte & 0x7F) > 1) {
            return false;
        }
        if (shift > 63) {
            return false;
        }
        value |= ((uint64_t)(byte & 0x7F)) << shift;

        if ((byte & 0x80) == 0) {
            break;
        }
        shift += 7;
    }
    *pos = p;
    if (out_value) {
        *out_value = value;
    }
    return true;
}

size_t xylem_varint_compute(uint64_t value) {
    size_t size = 1;
    while (value > 0x7F) {
        value >>= 7;
        size++;
    }
    return size;
}