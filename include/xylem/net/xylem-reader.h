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

typedef enum xylem_reader_transport_e {
    XYLEM_READER_TCP,
    XYLEM_READER_TLS,
    XYLEM_READER_UDS,
    XYLEM_READER_RUDP_STREAM,
    XYLEM_READER_MUX
} xylem_reader_transport_t;

/**
 * Buffered reader over any byte stream.
 *
 * Wraps a transport and provides read_full (exact byte count)
 * and read_until (delimiter scanning) on top of an internal buffer.
 */
typedef struct xylem_reader_s xylem_reader_t;

/**
 * @brief Create a buffered reader.
 *
 * @param conn       Transport connection handle.
 * @param transport  Transport type (determines read function).
 * @param size   Internal buffer size in bytes. Must be > 0.
 *
 * @return Reader handle, or NULL on failure.
 */
extern xylem_reader_t* xylem_reader_create(
    void*                    conn,
    xylem_reader_transport_t transport,
    int                      size);

/**
 * @brief Destroy a buffered reader.
 *
 * @param rd  Reader to destroy.
 */
extern void xylem_reader_destroy(xylem_reader_t* rd);

/**
 * @brief Read up to len bytes (read-some semantics).
 *
 * Returns available buffered data first. If the buffer is empty,
 * performs one read from the underlying transport.
 *
 * @param rd   Reader handle.
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 *
 * @return Bytes read (>0), 0 on EOF, negative on error.
 */
extern int xylem_reader_read(
    xylem_reader_t* rd,
    void*           buf,
    int             len);

/**
 * @brief Read exactly len bytes.
 *
 * Drains internal buffer first, then calls read_fn repeatedly
 * until len bytes are filled, EOF, or error.
 *
 * @param rd   Reader handle.
 * @param buf  Destination buffer.
 * @param len  Exact number of bytes to read.
 *
 * @return 0 on success, -1 on EOF or error before len bytes.
 */
extern int xylem_reader_read_full(
    xylem_reader_t* rd,
    void*           buf,
    int             len);

/**
 * @brief Read until delimiter is found.
 *
 * Scans for delim in the stream. The output includes the
 * delimiter byte. Returns -1 if len bytes are consumed
 * without finding the delimiter.
 *
 * @param rd     Reader handle.
 * @param delim  Delimiter byte to search for.
 * @param buf    Destination buffer.
 * @param len    Buffer size.
 *
 * @return Bytes written to buf (including delimiter), or -1 on
 *         error / EOF / overflow.
 */
extern int xylem_reader_read_until(
    xylem_reader_t* rd,
    uint8_t         delim,
    void*           buf,
    int             len);

/**
 * @brief Peek at the next len bytes without consuming them.
 *
 * Ensures the internal buffer contains at least len bytes
 * (reading from transport if needed). The peeked bytes
 * remain available for subsequent read calls.
 *
 * @param rd   Reader handle.
 * @param buf  Destination buffer.
 * @param len  Number of bytes to peek.
 *
 * @return 0 on success, -1 if len > cap or bytes not available.
 */
extern int xylem_reader_peek(
    xylem_reader_t* rd,
    void*           buf,
    int             len);
