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

#include "mux-frame.h"

void mux_frame_encode(_mux_frame_hdr_t* hdr, uint8_t buf[MUX_FRAME_HDR_SIZE]) {
    buf[0] = hdr->version;
    buf[1] = hdr->type;
    buf[2] = (uint8_t)(hdr->flags >> 8);
    buf[3] = (uint8_t)(hdr->flags);
    buf[4] = (uint8_t)(hdr->stream_id >> 24);
    buf[5] = (uint8_t)(hdr->stream_id >> 16);
    buf[6] = (uint8_t)(hdr->stream_id >> 8);
    buf[7] = (uint8_t)(hdr->stream_id);
    buf[8] = (uint8_t)(hdr->length >> 24);
    buf[9] = (uint8_t)(hdr->length >> 16);
    buf[10] = (uint8_t)(hdr->length >> 8);
    buf[11] = (uint8_t)(hdr->length);
}

void mux_frame_decode(const uint8_t buf[MUX_FRAME_HDR_SIZE], _mux_frame_hdr_t* hdr) {
    hdr->version   = buf[0];
    hdr->type      = buf[1];
    hdr->flags     = (uint16_t)((uint16_t)buf[2] << 8 | buf[3]);
    hdr->stream_id = (uint32_t)buf[4] << 24 | (uint32_t)buf[5] << 16
                     | (uint32_t)buf[6] << 8 | (uint32_t)buf[7];
    hdr->length    = (uint32_t)buf[8] << 24 | (uint32_t)buf[9] << 16
                     | (uint32_t)buf[10] << 8 | (uint32_t)buf[11];
}
