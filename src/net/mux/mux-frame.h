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

_Pragma("once")

#include <stdint.h>
#include <stddef.h>

#define MUX_FRAME_HDR_SIZE 12
#define MUX_PROTO_VERSION  0

typedef enum {
    MUX_TYPE_DATA          = 0,
    MUX_TYPE_WINDOW_UPDATE = 1,
    MUX_TYPE_PING          = 2,
    MUX_TYPE_GO_AWAY       = 3
} _mux_frame_type_t;

typedef enum {
    MUX_FLAG_SYN = 0x0001,
    MUX_FLAG_ACK = 0x0002,
    MUX_FLAG_FIN = 0x0004,
    MUX_FLAG_RST = 0x0008
} _mux_frame_flag_t;

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint32_t stream_id;
    uint32_t length;
} _mux_frame_hdr_t;

/**
 * @brief Serialize a frame header to wire format (big-endian).
 *
 * @param hdr  Header to encode.
 * @param buf  Output buffer, must be at least MUX_FRAME_HDR_SIZE bytes.
 */
extern void mux_frame_encode(_mux_frame_hdr_t* hdr, uint8_t buf[MUX_FRAME_HDR_SIZE]);

/**
 * @brief Deserialize a wire-format buffer into a frame header.
 *
 * @param buf  Input buffer of MUX_FRAME_HDR_SIZE bytes.
 * @param hdr  Output header struct.
 */
extern void mux_frame_decode(const uint8_t buf[MUX_FRAME_HDR_SIZE], _mux_frame_hdr_t* hdr);
