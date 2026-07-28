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

#include "xylem/net/xylem-tls.h"

#include "platform/platform-socket.h"

#include <stddef.h>
#include <stdint.h>

typedef xylem_tls_ctx_t      tls_ctx_t;
typedef xylem_tls_conn_t     tls_conn_t;
typedef xylem_tls_listener_t tls_listener_t;

/**
 * @brief Connect to a remote TLS endpoint and complete the handshake.
 *
 * @param host  Remote hostname or IP to connect to.
 * @param port  Remote port.
 * @param ctx   TLS context.
 * @param opts  TLS options (server_name, timeouts), or NULL.
 *
 * @return Connection handle, or NULL on failure or timeout.
 */
extern tls_conn_t* tls_dial(
    const char*       host,
    uint16_t          port,
    tls_ctx_t*        ctx,
    xylem_tls_opts_t* opts);

/**
 * @brief Close a connection and interrupt blocked operations.
 *
 * Does not release the owner handle.
 *
 * @param tls  Connection handle.
 */
extern void tls_close(tls_conn_t* tls);

/**
 * @brief Destroy a TLS connection. NULL-safe.
 *
 * Closes the connection if needed, then releases the owner handle.
 *
 * @param tls  Connection handle, or NULL.
 */
extern void tls_destroy(tls_conn_t* tls);

/**
 * @brief Create a TLS listener bound to the given address.
 *
 * @param host  Bind host (e.g. "0.0.0.0"), or NULL for any.
 * @param port  Bind port.
 * @param ctx   TLS context with cert+key loaded.
 * @param opts  TLS options, or NULL for defaults.
 *
 * @return Listener handle, or NULL on failure.
 */
extern tls_listener_t* tls_listen(
    const char*       host,
    uint16_t          port,
    tls_ctx_t*        ctx,
    xylem_tls_opts_t* opts);

/**
 * @brief Accept the next connection (handshake deferred).
 *
 * Returns as soon as a connection is accepted, WITHOUT running its TLS
 * handshake. The handshake is driven lazily on the first
 * tls_read/tls_write, or eagerly via tls_handshake(), inside the
 * per-connection handler coroutine -- so handshakes parallelize across
 * the scheduler instead of serializing behind this acceptor.
 *
 * Consequences:
 *  - NULL is returned when the listener is closed or an unrecoverable
 *    accept or allocation error occurs. It does not signal a handshake
 *    failure.
 *  - A handshake failure (bad cert, protocol mismatch, timeout) surfaces
 *    as -1 from the first tls_read/tls_write/tls_handshake, not here.
 *  - No server handshake timeout is installed automatically. Set both
 *    read and write deadlines before tls_handshake or the first I/O when
 *    the application needs to bound the handshake.
 *
 * Must be called from a single coroutine per listener: the accept parks
 * on the listener stream core, which permits only one accept parker (a
 * second concurrent accept aborts). To accept in parallel across cores,
 * give each worker its own listener on the same port and rely on
 * SO_REUSEPORT load balancing -- effective on Linux/macOS only; on
 * Windows reuseport is a no-op, so use a single acceptor there.
 *
 * @param ln  Listener handle.
 *
 * @return Accepted connection, or NULL when closed or on accept error.
 */
extern tls_conn_t*     tls_accept(tls_listener_t* ln);

/**
 * @brief Close a listener and interrupt blocked accept.
 *
 * Does not release the owner handle.
 *
 * @param ln  Listener handle.
 */
extern void tls_close_listener(tls_listener_t* ln);

/**
 * @brief Destroy a TLS listener. NULL-safe.
 *
 * Closes the listener if needed, then releases the owner handle.
 *
 * @param ln  Listener handle, or NULL.
 */
extern void tls_destroy_listener(tls_listener_t* ln);

/**
 * @brief Read available plaintext (read-some semantics).
 *
 * @param tls  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 *
 * @return Bytes read (>0), 0 on peer close, -1 on error/timeout.
 */
extern int tls_read(tls_conn_t* tls, void* buf, int len);

/**
 * @brief Write all of the buffer to the connection.
 *
 * @param tls   Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to write.
 *
 * @return 0 on success, -1 on error or timeout.
 */
extern int tls_write(tls_conn_t* tls, const void* data, int len);

/**
 * @brief Set the absolute read deadline.
 *
 * The deadline applies to subsequent reads and to a lazy server handshake
 * that is already in progress. Calling this function during that handshake
 * replaces the handshake's current read deadline.
 *
 * @param tls          Connection handle.
 * @param deadline_ms  xylem_utils_getnow(MSEC) deadline, or 0 to clear.
 */
extern void tls_set_read_deadline(tls_conn_t* tls, uint64_t deadline_ms);

/**
 * @brief Set the absolute write deadline.
 *
 * The deadline applies to subsequent writes and to a lazy server handshake
 * that is already in progress. Calling this function during that handshake
 * replaces the handshake's current write deadline.
 *
 * @param tls          Connection handle.
 * @param deadline_ms  xylem_utils_getnow(MSEC) deadline, or 0 to clear.
 */
extern void tls_set_write_deadline(tls_conn_t* tls, uint64_t deadline_ms);

/**
 * @brief Get the remote address of the connection.
 *
 * @param tls       Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer.
 * @param port      Receives the remote port.
 *
 * @return 0 on success, -1 on error.
 */
extern int tls_remote_addr(
    tls_conn_t* tls,
    char*       host,
    size_t      host_len,
    uint16_t*   port);

/**
 * @brief Get the local address of the connection.
 *
 * @param tls       Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer.
 * @param port      Receives the local port.
 *
 * @return 0 on success, -1 on error.
 */
extern int tls_local_addr(
    tls_conn_t* tls,
    char*       host,
    size_t      host_len,
    uint16_t*   port);

/**
 * @brief Get the local address of the listener.
 *
 * @param ln        Listener handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer.
 * @param port      Receives the local port.
 *
 * @return 0 on success, -1 on error.
 */
extern int tls_listener_addr(
    tls_listener_t* ln,
    char*           host,
    size_t          host_len,
    uint16_t*       port);

/**
 * @brief Get the negotiated ALPN protocol.
 *
 * @param tls  Connection handle.
 *
 * @return Protocol string, or NULL if none negotiated.
 */
extern const char* tls_get_alpn(tls_conn_t* tls);

/**
 * @brief Force the (lazy server) handshake to complete.
 *
 * A server connection returned by tls_accept is handed back before its
 * TLS handshake runs; the handshake is otherwise driven lazily on the
 * first tls_read/tls_write. Call this to drive it explicitly -- e.g.
 * before reading the negotiated ALPN (tls_get_alpn) or the peer
 * certificate. No-op (returns 0) for client connections and for server
 * connections already handshaked.
 *
 * Drives both stream directions, so exactly one coroutine owns it; a
 * second concurrent caller blocks until the driver finishes, then sees
 * the same result. Must be called under no rd_mu/wr_mu/ssl_mu hold.
 *
 * @param tls  Connection handle.
 *
 * @return 0 once the handshake has completed, -1 on handshake failure,
 *         timeout, or a closed connection.
 */
extern int tls_handshake(tls_conn_t* tls);

/**
 * @brief Perform a client-side TLS handshake on an already-connected fd.
 *
 * Wraps a socket whose transport connection is already established (e.g.
 * one obtained from an HTTP CONNECT proxy tunnel) and drives the TLS
 * client handshake to completion. SNI and certificate identity checks
 * use opts->server_name, which need not match the address the fd is
 * connected to -- exactly the proxy case (connect to proxy, verify the
 * target).
 * opts->connect_timeout_ms bounds only the TLS handshake because the
 * transport connection is already established.
 *
 * On success ownership of @p fd is transferred to the returned
 * connection; on failure @p fd is closed.
 *
 * @param fd   Connected socket (ownership transferred on success).
 * @param ctx  TLS context.
 * @param opts TLS options (server_name, connect_timeout_ms, etc.).
 *
 * @return TLS connection handle, or NULL on failure (fd is closed).
 */
extern tls_conn_t* tls_client_handshake_fd(
    platform_sock_t   fd,
    tls_ctx_t*        ctx,
    xylem_tls_opts_t* opts);
