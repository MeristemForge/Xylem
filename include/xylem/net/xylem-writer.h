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

typedef enum xylem_writer_transport_e {
    XYLEM_WRITER_TCP,
    XYLEM_WRITER_TLS,
    XYLEM_WRITER_UDS,
    XYLEM_WRITER_RUDP_STREAM,
    XYLEM_WRITER_MUX
} xylem_writer_transport_t;

/**
 * Buffered writer over any byte stream.
 *
 * Batches multiple small writes into fewer transport calls.
 */
typedef struct xylem_writer_s xylem_writer_t;

/**
 * @brief Create a buffered writer.
 *
 * Coroutine-only.
 *
 * @param conn       Transport connection handle.
 * @param transport  Transport type (determines write function).
 * @param size   Internal buffer size in bytes. Must be > 0.
 *
 * @return Writer handle, or NULL on failure.
 */
extern xylem_writer_t* xylem_writer_create(
    void*                    conn,
    xylem_writer_transport_t transport,
    int                      size);

/**
 * @brief Destroy a buffered writer.
 *
 * Coroutine-only.
 *
 * Flushes pending data before releasing resources.
 *
 * @param wr  Writer to destroy.
 */
extern void xylem_writer_destroy(xylem_writer_t* wr);

/**
 * @brief Write data through the buffer.
 *
 * Coroutine-only.
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
 * Coroutine-only.
 *
 * No-op if the buffer is empty.
 *
 * @param wr  Writer handle.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_writer_flush(xylem_writer_t* wr);
