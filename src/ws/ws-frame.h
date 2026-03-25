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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Parsed WebSocket frame header (RFC 6455 section 5.2). */
typedef struct {
    bool     fin;
    uint8_t  opcode;
    bool     masked;
    uint64_t payload_len;
    uint8_t  mask_key[4];
    size_t   header_size; /**< Total header bytes (2-14). */
} ws_frame_header_t;

/**
 * @brief Decode a WebSocket frame header from raw bytes.
 *
 * Parses the first bytes of a WebSocket frame to extract the header
 * fields. Validates reserved opcodes (3-7, 0xB-0xF), control frame
 * payload length (must be <= 125), and control frame FIN bit (must
 * be set).
 *
 * @param data  Pointer to the raw frame bytes.
 * @param len   Number of available bytes.
 * @param out   Pointer to the header struct to populate on success.
 *
 * @return 0 on success, -1 if insufficient data, -2 on protocol error.
 */
extern int ws_frame_decode_header(const uint8_t* data, size_t len,
                                  ws_frame_header_t* out);

/**
 * @brief Encode a WebSocket frame header into a buffer.
 *
 * Writes the frame header bytes for the given parameters. The caller
 * must ensure buf has at least 14 bytes of space (maximum header size).
 *
 * @param buf          Destination buffer (at least 14 bytes).
 * @param fin          FIN bit value.
 * @param opcode       Frame opcode (4 bits).
 * @param masked       Whether the frame is masked.
 * @param mask_key     4-byte masking key (used only when masked is true).
 * @param payload_len  Payload length to encode.
 *
 * @return Number of bytes written to buf.
 */
extern size_t ws_frame_encode_header(uint8_t* buf, bool fin,
                                     uint8_t opcode, bool masked,
                                     const uint8_t mask_key[4],
                                     uint64_t payload_len);

/**
 * @brief Apply or remove the WebSocket masking transform in-place.
 *
 * XORs each byte of data with the corresponding byte of the 4-byte
 * mask key, cycling from the given offset. Applying the mask twice
 * with the same key and offset restores the original data.
 *
 * @param data      Pointer to the payload bytes to mask/unmask.
 * @param len       Number of bytes to process.
 * @param mask_key  4-byte masking key.
 * @param offset    Starting offset into the mask key cycle.
 */
extern void ws_frame_apply_mask(uint8_t* data, size_t len,
                                const uint8_t mask_key[4], size_t offset);

/**
 * @brief Validate a close status code for sending.
 *
 * Checks whether the code falls within the ranges that an application
 * is allowed to send: 1000-1003, 1007-1011, or 3000-4999.
 *
 * @param code  The status code to validate.
 *
 * @return 0 if the code is valid for sending, -1 if invalid.
 */
extern int ws_frame_close_validate_send(uint16_t code);

/**
 * @brief Validate a close status code received from a peer.
 *
 * Rejects codes in reserved ranges that must not appear in close
 * frames on the wire: 0-999, 1004-1006, 1015, and 1016-2999.
 *
 * @param code  The status code to validate.
 *
 * @return 0 if the code is acceptable, -1 if reserved/invalid.
 */
extern int ws_frame_close_validate_recv(uint16_t code);

/**
 * @brief Encode a close frame payload.
 *
 * Writes the 2-byte status code in network byte order followed by
 * the reason string into the output buffer.
 *
 * @param code        The close status code.
 * @param reason      Pointer to the reason string (may be NULL if
 *                    reason_len is 0).
 * @param reason_len  Length of the reason string in bytes (max 123).
 * @param out         Destination buffer.
 * @param out_size    Size of the destination buffer in bytes.
 *
 * @return Number of bytes written on success, or -1 on error
 *         (reason too long or buffer too small).
 */
extern int ws_frame_close_encode(uint16_t code, const char* reason,
                                 size_t reason_len, uint8_t* out,
                                 size_t out_size);

/**
 * @brief Decode a close frame payload.
 *
 * Parses the close frame body according to RFC 6455 section 5.5.1:
 * - 0 bytes: valid, code is set to 1005 (No Status Received).
 * - 1 byte: protocol error.
 * - >= 2 bytes: extracts the 2-byte network byte order status code
 *   and points reason/reason_len at the remaining bytes.
 *
 * @param data        Pointer to the close frame payload bytes.
 * @param len         Length of the payload in bytes.
 * @param code        Receives the parsed status code (1005 when empty).
 * @param reason      Receives a pointer into data for the reason string
 *                    (not null-terminated). Set to NULL when no reason.
 * @param reason_len  Receives the length of the reason string.
 *
 * @return 0 on success, -1 on protocol error (1-byte payload),
 *         -2 if the status code is in a reserved range.
 */
extern int ws_frame_close_decode(const uint8_t* data, size_t len,
                                 uint16_t* code, const char** reason,
                                 size_t* reason_len);
