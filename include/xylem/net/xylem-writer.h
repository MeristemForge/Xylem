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
 * Write function signature for the underlying transport.
 * Must write all len bytes or return error.
 * Returns 0 on success, -1 on error.
 */
typedef int (*xylem_writer_fn_t)(void* ctx, const void* data, int len);

/**
 * Buffered writer over any byte stream.
 *
 * Batches multiple small writes into fewer transport calls.
 * Caller owns the buffer memory.
 */
typedef struct xylem_writer_s {
    void*             ctx;      /*< Opaque transport context. */
    xylem_writer_fn_t write_fn; /*< Underlying write-some function. */
    uint8_t*          buf;      /*< Internal buffer (caller-provided). */
    int               cap;      /*< Buffer capacity in bytes. */
    int               w;        /*< Bytes buffered so far. */
} xylem_writer_t;

/**
 * @brief Initialize a buffered writer.
 *
 * @param wr        Writer to initialize.
 * @param ctx       Opaque context passed to write_fn.
 * @param write_fn  Underlying write function (write-some semantics).
 * @param buf       Caller-provided buffer.
 * @param cap       Buffer capacity in bytes. Must be > 0.
 */
extern void xylem_writer_init(
    xylem_writer_t*   wr,
    void*             ctx,
    xylem_writer_fn_t write_fn,
    void*             buf,
    int               cap);

/**
 * @brief Reset writer state. Does not free the buffer.
 *
 * Discards any unflushed data.
 *
 * @param wr  Writer to reset.
 */
extern void xylem_writer_deinit(xylem_writer_t* wr);

/**
 * @brief Write data through the buffer.
 *
 * Small writes are buffered. When the buffer is full it is
 * flushed automatically. Writes larger than the buffer
 * bypass it entirely (after flushing pending data).
 *
 * @param wr    Writer handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to write.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_writer_write(
    xylem_writer_t* wr,
    const void*     data,
    int             len);

/**
 * @brief Flush buffered data to the transport.
 *
 * No-op if the buffer is empty.
 *
 * @param wr  Writer handle.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_writer_flush(xylem_writer_t* wr);
