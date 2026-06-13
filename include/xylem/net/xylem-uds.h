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

typedef struct xylem_uds_conn_s     xylem_uds_conn_t;
typedef struct xylem_uds_listener_s xylem_uds_listener_t;

/**
 * @brief Create a UDS listener bound to the given path.
 *
 * @note [COROUTINE-ONLY]
 *
 * Binds to the specified filesystem path, sets non-blocking mode,
 * and registers with the event loop. Unlinks the path first if it
 * already exists.
 *
 * @param path  Unix domain socket path.
 *
 * @return Listener handle, or NULL on failure.
 */
extern xylem_uds_listener_t* xylem_uds_listen(const char* path);

/**
 * @brief Accept a connection from the listener.
 *
 * @note [COROUTINE-ONLY]
 *
 * Suspends the calling coroutine until a client connects.
 *
 * @param ln  Listener handle.
 *
 * @return Accepted connection, or NULL if the listener is closed.
 */
extern xylem_uds_conn_t* xylem_uds_accept(xylem_uds_listener_t* ln);

/**
 * @brief Close and destroy a listener. Idempotent.
 *
 * @note [COROUTINE-ONLY]
 *
 * Wakes any coroutine blocked in xylem_uds_accept().
 * Unlinks the socket path from the filesystem.
 *
 * @param ln  Listener handle.
 */
extern void xylem_uds_close_listener(xylem_uds_listener_t* ln);

/**
 * @brief Connect to a Unix domain socket.
 *
 * @note [COROUTINE-ONLY]
 *
 * Suspends the calling coroutine until the connection is established
 * or connect_timeout_ms elapses.
 *
 * @param path                Unix domain socket path.
 * @param connect_timeout_ms  Connect timeout in ms, 0 = no timeout.
 *
 * @return Connection handle, or NULL on failure or timeout.
 */
extern xylem_uds_conn_t* xylem_uds_dial(
    const char* path,
    uint64_t    connect_timeout_ms);

/**
 * @brief Set the read deadline for the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Once the clock passes the deadline, in-flight and subsequent
 * xylem_uds_read() calls return -1.
 *
 * @param uds          Connection handle.
 * @param deadline_ms  Absolute monotonic timestamp in ms, or 0
 *                     to clear.
 */
extern void xylem_uds_set_read_deadline(
    xylem_uds_conn_t* uds,
    uint64_t          deadline_ms);

/**
 * @brief Set the write deadline for the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Mirror of xylem_uds_set_read_deadline for the write direction.
 *
 * @param uds          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void xylem_uds_set_write_deadline(
    xylem_uds_conn_t* uds,
    uint64_t          deadline_ms);

/**
 * @brief Read data from the connection (read-some semantics).
 *
 * @note [COROUTINE-ONLY]
 *
 * Returns available data from the socket. Suspends the calling
 * coroutine if no data is immediately available. At most len
 * bytes are returned; the actual count may be less.
 *
 * @param uds  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 *
 * @return Bytes read (>0), 0 on peer close, -1 on error/timeout.
 */
extern int xylem_uds_read(
    xylem_uds_conn_t* uds,
    void*             buf,
    int               len);

/**
 * @brief Write all data to the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Loops internally until all len bytes are sent or an error
 * occurs. Suspends the calling coroutine as needed.
 *
 * @param uds   Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to write.
 *
 * @return 0 on success, -1 on error or timeout.
 */
extern int xylem_uds_write(
    xylem_uds_conn_t* uds,
    const void*       data,
    int               len);

/**
 * @brief Close a connection. Idempotent.
 *
 * @note [COROUTINE-ONLY]
 *
 * Wakes any coroutine blocked in read/write.
 *
 * @param uds  Connection handle.
 */
extern void xylem_uds_close(xylem_uds_conn_t* uds);

/**
 * @brief Shut down the write side of the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Sends a FIN to the peer, signalling that no more data will be
 * written. The connection remains readable; the peer sees EOF on
 * their next read. Use this for graceful half-close protocols.
 *
 * @param uds  Connection handle.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_uds_shutdown_wr(xylem_uds_conn_t* uds);

/**
 * @brief Shut down the read side of the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Discards further incoming data. Subsequent read calls return -1.
 *
 * @param uds  Connection handle.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_uds_shutdown_rd(xylem_uds_conn_t* uds);
