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

#include "xylem/net/xylem-framing.h"

typedef struct xylem_uds_conn_s     xylem_uds_conn_t;
typedef struct xylem_uds_listener_s xylem_uds_listener_t;

/**
 * @brief Create a UDS listener bound to the given path.
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
 * Wakes any coroutine blocked in xylem_uds_accept().
 * Unlinks the socket path from the filesystem.
 *
 * @param ln  Listener handle.
 */
extern void xylem_uds_close_listener(xylem_uds_listener_t* ln);

/**
 * @brief Connect to a Unix domain socket.
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
 * @brief Set the framing mode for subsequent recv/send calls.
 *
 * @param uds   Connection handle.
 * @param opts  Frame options, NULL to reset to raw mode.
 */
extern void xylem_uds_set_framing(
    xylem_uds_conn_t*       uds,
    xylem_framing_opts_t* opts);

/**
 * @brief Set the read deadline for the connection.
 *
 * Once the clock passes the deadline, in-flight and subsequent
 * xylem_uds_recv() calls return -1.
 *
 * @param uds          Connection handle.
 * @param deadline_ms  Absolute monotonic timestamp in ms, or 0
 *                     to clear.
 */
extern void xylem_uds_set_read_deadline(
    xylem_uds_conn_t* uds,
    uint64_t           deadline_ms);

/**
 * @brief Set the write deadline for the connection.
 *
 * Mirror of xylem_uds_set_read_deadline for the write direction.
 *
 * @param uds          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void xylem_uds_set_write_deadline(
    xylem_uds_conn_t* uds,
    uint64_t           deadline_ms);

/**
 * @brief Receive data or a complete frame from the connection.
 *
 * Behavior depends on the configured framing mode:
 *   - NONE:      returns 1~len available bytes (buffered read).
 *   - FIXED:     returns exactly frame_opts.fixed.len bytes.
 *   - LENGTH:    reads header, decodes length, returns payload.
 *   - DELIMITER: reads until delimiter, returns data without it.
 *
 * @param uds  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Buffer size.
 *
 * @return Bytes received (>0), 0 if peer closed gracefully,
 *         -1 on error.
 */
extern int64_t xylem_uds_recv(
    xylem_uds_conn_t* uds,
    void*              buf,
    size_t             len);

/**
 * @brief Send data or a framed message to the connection.
 *
 * Behavior depends on the configured framing mode:
 *   - NONE/FIXED/DELIMITER: sends raw bytes, no framing applied.
 *   - LENGTH: prepends the encoded length header before data.
 *
 * All bytes are written before returning (loops internally until
 * the full buffer is sent or an error occurs).
 *
 * @param uds   Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to send.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_uds_send(
    xylem_uds_conn_t* uds,
    const void*        data,
    size_t             len);

/**
 * @brief Close a connection. Idempotent.
 *
 * Wakes any coroutine blocked in recv/send.
 *
 * @param uds  Connection handle.
 */
extern void xylem_uds_close(xylem_uds_conn_t* uds);

/**
 * @brief Shut down the write side of the connection.
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
 * Discards further incoming data. Subsequent recv calls return -1.
 *
 * @param uds  Connection handle.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_uds_shutdown_rd(xylem_uds_conn_t* uds);
