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

/**
 * @brief Compute the maximum encoded size for URL percent-encoding.
 *
 * Worst case: every byte encodes to 3 characters (%XX).
 *
 * @param slen  Length of the input data in bytes.
 *
 * @return Maximum encoded output size in bytes (excluding null terminator).
 */
extern int xylem_url_encode_size(int slen);

/**
 * @brief Compute the maximum decoded size for URL percent-decoding.
 *
 * Upper bound: decoded output is never larger than the encoded input.
 *
 * @param slen  Length of the percent-encoded input in bytes.
 *
 * @return Maximum decoded output size in bytes.
 */
extern int xylem_url_decode_size(int slen);

/**
 * @brief Percent-encode a string per RFC 3986.
 *
 * Unreserved characters (A-Z, a-z, 0-9, '-', '_', '.', '~') pass through;
 * all others become %XX.
 *
 * @param src   Input bytes.
 * @param slen  Input length in bytes.
 * @param dst   Output buffer (not null-terminated).
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of bytes written to dst on success; -1 if dlen is insufficient.
 */
extern int xylem_url_encode(const uint8_t* src, int slen, uint8_t* dst, int dlen);

/**
 * @brief Decode a percent-encoded string.
 *
 * Converts %XX sequences back to bytes. Invalid sequences pass through
 * unchanged.
 *
 * @param src   Percent-encoded input.
 * @param slen  Input length in bytes.
 * @param dst   Output buffer for decoded data.
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of decoded bytes on success; -1 if dlen is insufficient.
 */
extern int xylem_url_decode(const uint8_t* src, int slen, uint8_t* dst, int dlen);
