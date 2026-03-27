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
#include <stdint.h>

/**
 * @brief Compute the maximum compressed size for gzip format.
 *
 * Returns a conservative upper bound on the output size of
 * xylem_gzip_compress() for a given input length. Use this to allocate
 * the destination buffer.
 *
 * @param slen  Length of the uncompressed input in bytes.
 *
 * @return Upper bound on the compressed output size in bytes.
 */
extern size_t xylem_gzip_compress_bound(size_t slen);

/**
 * @brief Compress data using gzip format (RFC 1952).
 *
 * Produces a complete gzip stream with header, compressed payload, CRC-32,
 * and original-size trailer into the caller-provided buffer.
 *
 * @param src    Pointer to the input data.
 * @param slen   Length of the input data in bytes.
 * @param dst    Output buffer for the compressed data.
 * @param dlen   Size of the output buffer in bytes.
 * @param level  Compression level (0 = none, 1 = fastest, 9 = best,
 *               -1 = default).
 *
 * @return Number of bytes written to dst on success; -1 if dlen is
 *         insufficient or compression fails.
 *
 * @note Use xylem_gzip_compress_bound() to determine the required dlen.
 */
extern int xylem_gzip_compress(const uint8_t *src, size_t slen, uint8_t *dst,
                               size_t dlen, int level);

/**
 * @brief Decompress gzip-formatted data (RFC 1952).
 *
 * Parses the gzip header, inflates the payload, and verifies the CRC-32
 * trailer. Output is written into the caller-provided buffer.
 *
 * @param src   Pointer to the gzip-compressed data.
 * @param slen  Length of the compressed data in bytes.
 * @param dst   Output buffer for the decompressed data.
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of decompressed bytes written to dst on success; -1 on
 *         error (invalid data, CRC mismatch, or insufficient dlen).
 */
extern int xylem_gzip_decompress(const uint8_t *src, size_t slen, uint8_t *dst,
                                 size_t dlen);

/**
 * @brief Compute the maximum compressed size for raw DEFLATE.
 *
 * Returns a conservative upper bound on the output size of
 * xylem_gzip_deflate() for a given input length.
 *
 * @param slen  Length of the uncompressed input in bytes.
 *
 * @return Upper bound on the deflated output size in bytes.
 */
extern size_t xylem_gzip_deflate_bound(size_t slen);

/**
 * @brief Compress data using raw DEFLATE (RFC 1951).
 *
 * Produces a raw deflate stream without any header or trailer into the
 * caller-provided buffer.
 *
 * @param src    Pointer to the input data.
 * @param slen   Length of the input data in bytes.
 * @param dst    Output buffer for the deflated data.
 * @param dlen   Size of the output buffer in bytes.
 * @param level  Compression level (0 = none, 1 = fastest, 9 = best,
 *               -1 = default).
 *
 * @return Number of bytes written to dst on success; -1 if dlen is
 *         insufficient or compression fails.
 *
 * @note Use xylem_gzip_deflate_bound() to determine the required dlen.
 */
extern int xylem_gzip_deflate(const uint8_t *src, size_t slen, uint8_t *dst,
                              size_t dlen, int level);

/**
 * @brief Decompress raw DEFLATE data (RFC 1951).
 *
 * Inflates a raw deflate stream (no header or trailer) into the
 * caller-provided buffer.
 *
 * @param src   Pointer to the deflated data.
 * @param slen  Length of the deflated data in bytes.
 * @param dst   Output buffer for the inflated data.
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of decompressed bytes written to dst on success; -1 on
 *         error (invalid data or insufficient dlen).
 */
extern int xylem_gzip_inflate(const uint8_t *src, size_t slen, uint8_t *dst,
                              size_t dlen);
