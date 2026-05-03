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

typedef struct xylem_fec_s xylem_fec_t;

/**
 * @brief Create a Reed-Solomon FEC codec.
 *
 * @param data_shards    Number of data shards (1..254).
 * @param parity_shards  Number of parity shards (1..255-data_shards).
 *
 * @return Codec handle, or NULL if parameters are invalid or allocation fails.
 */
extern xylem_fec_t* xylem_fec_create(int data_shards, int parity_shards);

/**
 * @brief Destroy a FEC codec and free all associated memory.
 *
 * @param fec  Codec handle. NULL is safely ignored.
 */
extern void xylem_fec_destroy(xylem_fec_t* fec);

/**
 * @brief Encode parity shards from data shards.
 *
 * Computes parity_shards parity blocks from data_shards data blocks.
 * Each block must be exactly shard_size bytes.
 *
 * @param fec         Codec handle.
 * @param data        Array of data_shards pointers, each pointing to
 *                    shard_size bytes of input data.
 * @param parity      Array of parity_shards pointers, each pointing to
 *                    shard_size bytes of output buffer. Filled on success.
 * @param shard_size  Size of each shard in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_fec_encode(xylem_fec_t* fec, uint8_t** data,
                            uint8_t** parity, size_t shard_size);

/**
 * @brief Reconstruct lost data shards in-place.
 *
 * shards is [data_0 .. data_N-1, parity_0 .. parity_M-1]. marks indicates
 * which are lost (non-zero = lost). Only data shards are recovered; lost
 * parity shards are ignored (re-encode to regenerate them).
 *
 * @param fec         Codec handle.
 * @param shards      Array of (data_shards + parity_shards) pointers,
 *                    each pointing to shard_size bytes.
 * @param marks       Array of (data_shards + parity_shards) bytes.
 *                    Non-zero marks a shard as lost.
 * @param shard_size  Size of each shard in bytes.
 *
 * @return 0 on success, -1 on failure (too many losses or invalid input).
 */
extern int xylem_fec_reconstruct(xylem_fec_t* fec, uint8_t** shards,
                                 uint8_t* marks, size_t shard_size);
