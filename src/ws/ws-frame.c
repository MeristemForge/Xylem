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

#include "ws-frame.h"

#include <string.h>

/* Check whether an opcode falls in a reserved range. */
static bool _frame_is_reserved_opcode(uint8_t opcode) {
    return (opcode >= 0x3 && opcode <= 0x7) ||
           (opcode >= 0xB && opcode <= 0xF);
}

/* Check whether an opcode is a control frame (>= 0x8). */
static bool _frame_is_control(uint8_t opcode) {
    return opcode >= 0x8;
}

int ws_frame_decode_header(const uint8_t* data, size_t len,
                           ws_frame_header_t* out) {
    if (len < 2) {
        return -1;
    }

    uint8_t b0 = data[0];
    uint8_t b1 = data[1];

    out->fin    = (b0 & 0x80) != 0;
    out->opcode = b0 & 0x0F;
    out->masked = (b1 & 0x80) != 0;

    /* Reject reserved opcodes. */
    if (_frame_is_reserved_opcode(out->opcode)) {
        return -2;
    }

    uint8_t len7 = b1 & 0x7F;
    size_t  hdr  = 2;

    if (len7 <= 125) {
        out->payload_len = len7;
    } else if (len7 == 126) {
        hdr += 2;
        if (len < hdr) {
            return -1;
        }
        out->payload_len = ((uint64_t)data[2] << 8) | (uint64_t)data[3];
    } else {
        /* len7 == 127 */
        hdr += 8;
        if (len < hdr) {
            return -1;
        }
        out->payload_len = ((uint64_t)data[2] << 56) |
                           ((uint64_t)data[3] << 48) |
                           ((uint64_t)data[4] << 40) |
                           ((uint64_t)data[5] << 32) |
                           ((uint64_t)data[6] << 24) |
                           ((uint64_t)data[7] << 16) |
                           ((uint64_t)data[8] << 8)  |
                           ((uint64_t)data[9]);
    }

    if (out->masked) {
        hdr += 4;
        if (len < hdr) {
            return -1;
        }
        memcpy(out->mask_key, data + hdr - 4, 4);
    } else {
        memset(out->mask_key, 0, 4);
    }

    out->header_size = hdr;

    /* Control frame validations (RFC 6455 section 5.5). */
    if (_frame_is_control(out->opcode)) {
        if (out->payload_len > 125) {
            return -2;
        }
        if (!out->fin) {
            return -2;
        }
    }

    return 0;
}

size_t ws_frame_encode_header(uint8_t* buf, bool fin,
                              uint8_t opcode, bool masked,
                              const uint8_t mask_key[4],
                              uint64_t payload_len) {
    size_t pos = 0;

    buf[pos] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    pos++;

    uint8_t mask_bit = masked ? 0x80 : 0x00;

    if (payload_len <= 125) {
        buf[pos] = mask_bit | (uint8_t)payload_len;
        pos++;
    } else if (payload_len <= 65535) {
        buf[pos] = mask_bit | 126;
        pos++;
        buf[pos++] = (uint8_t)(payload_len >> 8);
        buf[pos++] = (uint8_t)(payload_len & 0xFF);
    } else {
        buf[pos] = mask_bit | 127;
        pos++;
        buf[pos++] = (uint8_t)(payload_len >> 56);
        buf[pos++] = (uint8_t)(payload_len >> 48);
        buf[pos++] = (uint8_t)(payload_len >> 40);
        buf[pos++] = (uint8_t)(payload_len >> 32);
        buf[pos++] = (uint8_t)(payload_len >> 24);
        buf[pos++] = (uint8_t)(payload_len >> 16);
        buf[pos++] = (uint8_t)(payload_len >> 8);
        buf[pos++] = (uint8_t)(payload_len);
    }

    if (masked) {
        memcpy(buf + pos, mask_key, 4);
        pos += 4;
    }

    return pos;
}

void ws_frame_apply_mask(uint8_t* data, size_t len,
                         const uint8_t mask_key[4], size_t offset) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= mask_key[(offset + i) & 3];
    }
}

int ws_frame_close_validate_send(uint16_t code) {
    if (code >= 1000 && code <= 1003) {
        return 0;
    }
    if (code >= 1007 && code <= 1011) {
        return 0;
    }
    if (code >= 3000 && code <= 4999) {
        return 0;
    }
    return -1;
}

int ws_frame_close_validate_recv(uint16_t code) {
    if (code <= 999) {
        return -1;
    }
    if (code >= 1004 && code <= 1006) {
        return -1;
    }
    if (code == 1015) {
        return -1;
    }
    if (code >= 1016 && code <= 2999) {
        return -1;
    }
    return 0;
}

int ws_frame_close_encode(uint16_t code, const char* reason,
                          size_t reason_len, uint8_t* out,
                          size_t out_size) {
    /* Max close frame payload: 2-byte code + 123-byte reason = 125 bytes. */
    if (reason_len > 123) {
        return -1;
    }

    size_t needed = 2 + reason_len;
    if (out_size < needed) {
        return -1;
    }

    /* Status code in network byte order (big-endian). */
    out[0] = (uint8_t)(code >> 8);
    out[1] = (uint8_t)(code & 0xFF);

    if (reason_len > 0 && reason != NULL) {
        memcpy(out + 2, reason, reason_len);
    }

    return (int)needed;
}

int ws_frame_close_decode(const uint8_t* data, size_t len,
                          uint16_t* code, const char** reason,
                          size_t* reason_len) {
    if (len == 0) {
        /* Empty close frame -> 1005 (No Status Received). */
        *code       = 1005;
        *reason     = NULL;
        *reason_len = 0;
        return 0;
    }

    if (len == 1) {
        /* 1-byte payload is a protocol error. */
        return -1;
    }

    /* >= 2 bytes: parse status code from network byte order. */
    *code = (uint16_t)((uint16_t)data[0] << 8 | (uint16_t)data[1]);

    if (len > 2) {
        *reason     = (const char*)(data + 2);
        *reason_len = len - 2;
    } else {
        *reason     = NULL;
        *reason_len = 0;
    }

    /* Check for reserved status codes. */
    if (ws_frame_close_validate_recv(*code) != 0) {
        return -2;
    }

    return 0;
}
