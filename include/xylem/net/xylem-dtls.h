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

typedef struct xylem_dtls_conn_s     xylem_dtls_conn_t;
typedef struct xylem_dtls_ctx_s      xylem_dtls_ctx_t;
typedef struct xylem_dtls_listener_s xylem_dtls_listener_t;

typedef struct xylem_dtls_opts_s {
    uint64_t    connect_timeout_ms; /*< Handshake timeout in ms, 0 = default 30s. */
    const char* hostname;           /*< SNI hostname for verification. */
} xylem_dtls_opts_t;

/**
 * @brief Create a DTLS context.
 *
 * Default: peer verification enabled, DTLS 1.2 minimum, cookie
 * exchange enabled for server-side anti-amplification.
 *
 * @return Context handle, or NULL on failure.
 */
extern xylem_dtls_ctx_t* xylem_dtls_ctx_create(void);

/**
 * @brief Destroy a DTLS context. NULL-safe.
 *
 * @param ctx  Context handle.
 */
extern void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx);

/**
 * @brief Load a PEM certificate chain and private key.
 *
 * @param ctx   Context handle.
 * @param cert  Path to PEM certificate chain file.
 * @param key   Path to PEM private key file.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_ctx_load_cert(xylem_dtls_ctx_t* ctx,
                                    const char* cert, const char* key);

/**
 * @brief Set the CA certificate for peer verification.
 *
 * @param ctx      Context handle.
 * @param ca_file  Path to CA certificate file.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_ctx_set_ca(xylem_dtls_ctx_t* ctx,
                                 const char* ca_file);

/**
 * @brief Enable or disable peer certificate verification.
 *
 * @param ctx     Context handle.
 * @param enable  true to verify, false to skip.
 */
extern void xylem_dtls_ctx_set_verify(xylem_dtls_ctx_t* ctx, bool enable);

/**
 * @brief Set the ALPN protocol list.
 *
 * @param ctx        Context handle.
 * @param protocols  Array of protocol strings (e.g. "h2", "http/1.1").
 * @param count      Number of protocols.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_ctx_set_alpn(xylem_dtls_ctx_t* ctx,
                                   const char** protocols, size_t count);

/**
 * @brief Enable NSS Key Log output for Wireshark decryption.
 *
 * @param ctx   Context handle.
 * @param path  Output file path, or NULL to disable.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx,
                                     const char* path);

/**
 * @brief Connect to a remote DTLS endpoint.
 *
 * Suspends the calling coroutine until the DTLS handshake completes
 * or connect_timeout_ms elapses (default 30s).
 *
 * @param host  Remote hostname or IP address.
 * @param port  Remote port.
 * @param ctx   DTLS context.
 * @param opts  DTLS options, NULL for defaults.
 *
 * @return Connection handle, or NULL on failure or timeout.
 */
extern xylem_dtls_conn_t* xylem_dtls_dial(
    const char*        host,
    uint16_t           port,
    xylem_dtls_ctx_t*  ctx,
    xylem_dtls_opts_t* opts);

/**
 * @brief Create a DTLS listener bound to the given address.
 *
 * Spawns an internal dispatcher coroutine that demultiplexes
 * incoming datagrams to per-peer sessions.
 *
 * @param host  Bind address (e.g. "0.0.0.0"), or NULL for any.
 * @param port  Bind port.
 * @param ctx   DTLS context with cert+key loaded.
 * @param opts  DTLS options, NULL for defaults.
 *
 * @return Listener handle, or NULL on failure.
 */
extern xylem_dtls_listener_t* xylem_dtls_listen(
    const char*        host,
    uint16_t           port,
    xylem_dtls_ctx_t*  ctx,
    xylem_dtls_opts_t* opts);

/**
 * @brief Accept a connection from the listener.
 *
 * Suspends the calling coroutine until a client completes the
 * DTLS handshake (including cookie exchange).
 *
 * @param ln  Listener handle.
 *
 * @return Accepted connection, or NULL if the listener is closed.
 */
extern xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln);

/**
 * @brief Receive a decrypted datagram from the connection.
 *
 * Suspends the calling coroutine until data arrives, the read
 * deadline passes, or the connection is closed.
 *
 * @param dtls  Connection handle.
 * @param buf   Destination buffer.
 * @param len   Buffer size.
 *
 * @return Bytes received (>0), 0 on peer close, -1 on error.
 */
extern int64_t xylem_dtls_recv(
    xylem_dtls_conn_t* dtls,
    void*              buf,
    size_t             len);

/**
 * @brief Send an encrypted datagram on the connection.
 *
 * Suspends the calling coroutine if the socket buffer is full
 * until writable, the write deadline passes, or the connection
 * is closed.
 *
 * @param dtls  Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to send.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_dtls_send(
    xylem_dtls_conn_t* dtls,
    const void*        data,
    size_t             len);

/**
 * @brief Set the read deadline for the connection.
 *
 * Once the clock passes the deadline, in-flight and subsequent
 * xylem_dtls_recv() calls return -1.
 *
 * @param dtls         Connection handle.
 * @param deadline_ms  Absolute monotonic timestamp in ms, or 0
 *                     to clear.
 */
extern void xylem_dtls_set_read_deadline(
    xylem_dtls_conn_t* dtls,
    uint64_t           deadline_ms);

/**
 * @brief Set the write deadline for the connection.
 *
 * Once the clock passes the deadline, in-flight and subsequent
 * xylem_dtls_send() calls return -1.
 *
 * @param dtls         Connection handle.
 * @param deadline_ms  Absolute monotonic timestamp in ms, or 0
 *                     to clear.
 */
extern void xylem_dtls_set_write_deadline(
    xylem_dtls_conn_t* dtls,
    uint64_t           deadline_ms);

/**
 * @brief Close a connection. Idempotent.
 *
 * Wakes any coroutine blocked in recv/send.
 *
 * @param dtls  Connection handle.
 */
extern void xylem_dtls_close(xylem_dtls_conn_t* dtls);

/**
 * @brief Close and destroy a listener. Idempotent.
 *
 * Closes all active sessions, stops the dispatcher coroutine,
 * and frees all resources.
 *
 * @param ln  Listener handle.
 */
extern void xylem_dtls_close_listener(xylem_dtls_listener_t* ln);

/**
 * @brief Get the negotiated ALPN protocol.
 *
 * @param dtls  Connection handle.
 *
 * @return Protocol string, or NULL if none negotiated.
 */
extern const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls);

/**
 * @brief Get the remote address of the connection.
 *
 * @param dtls      Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer (46 bytes recommended).
 * @param port      Receives the remote port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_dtls_remote_addr(
    xylem_dtls_conn_t* dtls,
    char*              host,
    size_t             host_len,
    uint16_t*          port);

/**
 * @brief Get the local address of the connection.
 *
 * @param dtls      Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer (46 bytes recommended).
 * @param port      Receives the local port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_dtls_local_addr(
    xylem_dtls_conn_t* dtls,
    char*              host,
    size_t             host_len,
    uint16_t*          port);

/**
 * @brief Get the local address of the listener.
 *
 * @param ln        Listener handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer (46 bytes recommended).
 * @param port      Receives the local port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_dtls_listener_addr(
    xylem_dtls_listener_t* ln,
    char*                  host,
    size_t                 host_len,
    uint16_t*              port);
