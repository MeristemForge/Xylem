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

typedef struct xylem_sha1_s xylem_sha1_t;

/**
 * @brief Create and initialize a SHA-1 context.
 *
 * @return Pointer to the new context, or NULL on allocation failure.
 */
extern xylem_sha1_t* xylem_sha1_create(void);

/**
 * @brief Feed data into the SHA-1 context.
 *
 * Can be called multiple times to hash data incrementally.
 *
 * @param ctx   Pointer to the SHA-1 context.
 * @param data  Pointer to the input data.
 * @param len   Length of the input data in bytes.
 */
extern void xylem_sha1_update(xylem_sha1_t* ctx, const uint8_t* data, size_t len);

/**
 * @brief Finalize the hash and produce the 20-byte digest.
 *
 * The context should not be used for further updates after this call.
 *
 * @param ctx     Pointer to the SHA-1 context.
 * @param digest  Output buffer of at least 20 bytes.
 */
extern void xylem_sha1_final(xylem_sha1_t* ctx, uint8_t digest[20]);

/**
 * @brief Destroy a SHA-1 context and zero sensitive data.
 *
 * @param ctx  Pointer to the SHA-1 context.
 */
extern void xylem_sha1_destroy(xylem_sha1_t* ctx);