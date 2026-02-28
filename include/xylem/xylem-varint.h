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
 * @brief Compute the number of bytes needed to encode a value as a varint.
 *
 * @param value  The unsigned 64-bit integer to measure.
 * @return Number of bytes required (1 to 10).
 */
extern size_t xylem_varint_compute(uint64_t value);

/**
 * @brief Encode a 64-bit unsigned integer as a varint into a buffer.
 *
 * Writes the variable-length encoding of @p value starting at @p buf[*pos]
 * and advances @p pos past the written bytes.
 *
 * @param value    The value to encode.
 * @param buf      Output buffer.
 * @param bufsize  Total size of @p buf in bytes.
 * @param pos      In/out byte offset; updated to point past the encoded data.
 * @return true on success, false if the buffer has insufficient space.
 */
extern bool xylem_varint_encode(uint64_t value, uint8_t* buf, size_t bufsize, size_t* pos);

/**
 * @brief Decode a varint from a buffer into a 64-bit unsigned integer.
 *
 * Reads a variable-length encoded integer starting at @p buf[*pos] and
 * advances @p pos past the consumed bytes.
 *
 * @param buf        Input buffer containing the encoded varint.
 * @param bufsize    Total size of @p buf in bytes.
 * @param pos        In/out byte offset; updated to point past the decoded data.
 * @param out_value  Pointer to store the decoded value.
 * @return true on success, false on truncated or malformed input.
 */
extern bool xylem_varint_decode(const uint8_t* buf, size_t bufsize, size_t* pos, uint64_t* out_value);