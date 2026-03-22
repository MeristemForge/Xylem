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

#include <stddef.h>

/**
 * @brief Percent-encode a string for use in URL path or query.
 *
 * Encodes reserved and non-ASCII bytes as %XX sequences per RFC 3986.
 *
 * @param src      Source bytes.
 * @param src_len  Source length in bytes.
 * @param out_len  Output: encoded length excluding NUL terminator.
 *
 * @return Newly allocated encoded string, or NULL on failure.
 *
 * @note The caller must free the returned string with free().
 */
extern char* xylem_http_url_encode(const char* src, size_t src_len,
                                    size_t* out_len);

/**
 * @brief Decode a percent-encoded string.
 *
 * Decodes %XX sequences back to their original byte values.
 *
 * @param src      Source string.
 * @param src_len  Source length in bytes.
 * @param out_len  Output: decoded length.
 *
 * @return Newly allocated decoded string, or NULL on failure.
 *
 * @note The caller must free the returned string with free().
 */
extern char* xylem_http_url_decode(const char* src, size_t src_len,
                                    size_t* out_len);
