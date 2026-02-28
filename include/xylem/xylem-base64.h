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

#include "xylem.h"

/**
 * @brief Encode binary data using standard Base64 (RFC 4648 compliant).
 *
 * Encodes input bytes into a Base64 string using the standard alphabet
 * (A-Z, a-z, 0-9, '+', '/') and always appends '=' padding as needed.
 *
 * @param src   Pointer to the input binary data.
 * @param slen  Length of the input data in bytes.
 * @param dst   Output buffer to store the encoded Base64 string (no null terminator).
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of bytes written to `dst` on success; -1 if `dlen` is insufficient.
 *
 * @note The caller must ensure that:
 *       dlen >= ((slen + 2) / 3) * 4
 *       Examples: slen=3 -> dlen>=4; slen=1 -> dlen>=4; slen=0 -> dlen>=0.
 */
extern int xylem_base64_encode_std(const uint8_t* src, int slen, uint8_t* dst, int dlen);

/**
 * @brief Decode a standard Base64 string (RFC 4648 compliant).
 *
 * Decodes a Base64-encoded string (using '+', '/', and '=' padding) back to binary.
 * Input must consist only of valid Base64 characters and have a length that is
 * a multiple of 4 when padding is enabled.
 *
 * @param src   Pointer to the Base64-encoded string (may include '=' padding).
 * @param slen  Length of the input string in bytes.
 * @param dst   Output buffer for the decoded binary data.
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of decoded bytes on success; -1 on error (e.g., invalid character,
 *         incorrect padding, or insufficient `dlen`).
 *
 * @note The caller must ensure that:
 *       dlen >= ((slen + 3) / 4) * 3
 *       This is the maximum possible output size (even if the input contains padding).
 *       Examples: slen=4 -> dlen>=3; slen=8 -> dlen>=6.
 */
extern int xylem_base64_decode_std(const uint8_t* src, int slen, uint8_t* dst, int dlen);

/**
 * @brief Encode binary data using URL-safe Base64 (RFC 4648, Section 5).
 *
 * Encodes input bytes using the URL-safe alphabet (A-Z, a-z, 0-9, '-', '_').
 * Padding with '=' is optional and controlled by the `padding` parameter.
 *
 * @param src      Pointer to the input binary data.
 * @param slen     Length of the input data in bytes.
 * @param dst      Output buffer for the encoded string (no null terminator).
 * @param dlen     Size of the output buffer in bytes.
 * @param padding  If true, append '=' padding; otherwise, omit padding.
 *
 * @return Number of bytes written to `dst` on success; -1 if `dlen` is insufficient.
 *
 * @note The caller must ensure that:
 *       dlen >= ((slen + 2) / 3) * 4
 *       This upper bound applies regardless of the `padding` setting.
 */
extern int xylem_base64_encode_url(const uint8_t* src, int slen, uint8_t* dst, int dlen, bool padding);

/**
 * @brief Decode a URL-safe Base64 string (supports optional padding).
 *
 * Decodes a Base64 string encoded with the URL-safe alphabet ('-' and '_').
 * Supports both padded and unpadded inputs based on the `padding` flag.
 *
 * @param src      Pointer to the URL-safe Base64 string.
 * @param slen     Length of the input string in bytes.
 * @param dst      Output buffer for the decoded binary data.
 * @param dlen     Size of the output buffer in bytes.
 * @param padding  If true, enforce strict padding rules (input length must be
 *                 a multiple of 4, and '=' must appear only in valid positions).
 *                 If false, accept inputs without padding.
 *
 * @return Number of decoded bytes on success; -1 on error.
 *
 * @note The caller must ensure that:
 *       dlen >= ((slen + 3) / 4) * 3
 *       This represents the theoretical maximum output size for any valid input.
 */
extern int xylem_base64_decode_url(const uint8_t* src, int slen, uint8_t* dst, int dlen, bool padding);