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

#include "xylem/xylem-bswap.h"

#include <string.h>

uint16_t xylem_bswap_u16(uint16_t val) {
    return (val << 8) | (val >> 8);
}

uint32_t xylem_bswap_u32(uint32_t val) {
    return ((val << 24) & 0xFF000000) | ((val << 8) & 0x00FF0000) |
           ((val >> 8) & 0x0000FF00) | ((val >> 24) & 0x000000FF);
}

uint64_t xylem_bswap_u64(uint64_t val) {
    return ((val << 56) & 0xFF00000000000000ULL) |
           ((val << 40) & 0x00FF000000000000ULL) |
           ((val << 24) & 0x0000FF0000000000ULL) |
           ((val << 8) & 0x000000FF00000000ULL) |
           ((val >> 8) & 0x00000000FF000000ULL) |
           ((val >> 24) & 0x0000000000FF0000ULL) |
           ((val >> 40) & 0x000000000000FF00ULL) |
           ((val >> 56) & 0x00000000000000FFULL);
}

int16_t xylem_bswap_i16(int16_t val) {
    return (int16_t)xylem_bswap_u16((uint16_t)val);
}

int32_t xylem_bswap_i32(int32_t val) {
    return (int32_t)xylem_bswap_u32((uint32_t)val);
}

int64_t xylem_bswap_i64(int64_t val) {
    return (int64_t)xylem_bswap_u64((uint64_t)val);
}

float xylem_bswap_f32(float val) {
    uint32_t tmp;
    memcpy(&tmp, &val, sizeof(val));
    tmp = xylem_bswap_u32(tmp);
    memcpy(&val, &tmp, sizeof(val));
    return val;
}

double xylem_bswap_f64(double val) {
    uint64_t tmp;
    memcpy(&tmp, &val, sizeof(val));
    tmp = xylem_bswap_u64(tmp);
    memcpy(&val, &tmp, sizeof(val));
    return val;
}