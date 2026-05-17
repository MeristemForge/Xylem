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

#include "xylem/net/xylem-tcp.h"
#include "xylem/xylem-error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct xylem_tls_conn_s     xylem_tls_conn_t;
typedef struct xylem_tls_ctx_s      xylem_tls_ctx_t;
typedef struct xylem_tls_listener_s xylem_tls_listener_t;

typedef struct xylem_tls_opts_s {
    size_t      max_read_buf;       /*< Plaintext read buffer size, 0 = default 64KB. */
    bool        disable_mss_clamp;  /*< Disable MSS clamping on the socket. */
    uint64_t    connect_timeout_ms; /*< TCP connect + TLS handshake timeout, 0 = none. */
    const char* hostname;           /*< SNI hostname for certificate selection and verification. */
} xylem_tls_opts_t;

/**
 * @brief Create a TLS context.
 *
 * Default: peer verification enabled, TLS 1.2 minimum.
 *
 * @return Context handle, or NULL on failure.
 */
extern xylem_tls_ctx_t* xylem_tls_ctx_create(void);

/**
 * @brief Destroy a TLS context. NULL-safe.
 *
 * @param ctx  Context handle.
 */
extern void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx);

/**
 * @brief Load a PEM certificate chain and private key.
 *
 * @param ctx   Context handle.
 * @param cert  Path to PEM certificate chain file.
 * @param key   Path to PEM private key file.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                                   const char* cert, const char* key);

/**
 * @brief Set the CA certificate for peer verification.
 *
 * @param ctx      Context handle.
 * @param ca_file  Path to CA certificate file.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_file);

/**
 * @brief Enable or disable peer certificate verification.
 *
 * @param ctx     Context handle.
 * @param enable  true to verify, false to skip.
 */
extern void xylem_tls_ctx_set_verify(xylem_tls_ctx_t* ctx, bool enable);

/**
 * @brief Set the ALPN protocol list.
 *
 * @param ctx        Context handle.
 * @param protocols  Array of protocol strings (e.g. "h2", "http/1.1").
 * @param count      Number of protocols.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_ctx_set_alpn(xylem_tls_ctx_t* ctx,
                                  const char** protocols, size_t count);

/**
 * @brief Enable NSS Key Log output for Wireshark decryption.
 *
 * @param ctx   Context handle.
 * @param path  Output file path, or NULL to disable.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path);

/**
 * @brief Connect to a remote TLS endpoint.
 *
 * Suspends the calling coroutine until the TCP connection is established
 * and the TLS handshake completes, or connect_timeout_ms elapses.
 *
 * @param host  Remote hostname or IP address.
 * @param port  Remote port.
 * @param ctx   TLS context.
 * @param opts  TLS options, NULL for defaults.
 *
 * @return Connection handle, or NULL on failure or timeout.
 */
extern xylem_tls_conn_t* xylem_tls_dial(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts);

/**
 * @brief Create a TLS listener bound to the given address.
 *
 * @param host  Bind address (e.g. "0.0.0.0"), or NULL for any.
 * @param port  Bind port.
 * @param ctx   TLS context with cert+key loaded.
 * @param opts  TLS options, NULL for defaults.
 *
 * @return Listener handle, or NULL on failure.
 */
extern xylem_tls_listener_t* xylem_tls_listen(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts);

/**
 * @brief Accept a connection from the listener.
 *
 * Suspends the calling coroutine until a client connects and the
 * TLS handshake completes.
 *
 * @param ln  Listener handle.
 *
 * @return Accepted connection, or NULL if the listener is closing.
 */
extern xylem_tls_conn_t* xylem_tls_accept(xylem_tls_listener_t* ln);

/**
 * @brief Set the framing mode for subsequent recv/send calls.
 *
 * @param tls   Connection handle.
 * @param opts  Frame options, NULL to reset to raw mode.
 */
extern void xylem_tls_set_framing(
    xylem_tls_conn_t*       tls,
    xylem_tcp_frame_opts_t* opts);

/**
 * @brief Set the read deadline for the connection.
 *
 * @param tls          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void xylem_tls_set_read_deadline(
    xylem_tls_conn_t* tls,
    uint64_t          deadline_ms);

/**
 * @brief Set the write deadline for the connection.
 *
 * @param tls          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void xylem_tls_set_write_deadline(
    xylem_tls_conn_t* tls,
    uint64_t          deadline_ms);

/**
 * @brief Receive data or a complete frame from the connection.
 *
 * Behavior depends on the configured framing mode (same as TCP):
 *   - NONE:      returns 1~len available bytes.
 *   - FIXED:     returns exactly frame_opts.fixed.len bytes.
 *   - LENGTH:    reads header, decodes length, returns payload.
 *   - DELIMITER: reads until delimiter, returns data without it.
 *
 * @param tls  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Buffer size.
 *
 * @return Bytes read (>0), 0 on peer close (NONE mode, error set
 *         to XYLEM_ERR_PEER_CLOSED), -1 on error/timeout.
 */
extern int64_t xylem_tls_recv(
    xylem_tls_conn_t* tls,
    void*             buf,
    size_t            len);

/**
 * @brief Send data or a framed message to the connection.
 *
 * All bytes are written before returning.
 *
 * @param tls   Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to send.
 *
 * @return 0 on success, -1 on error or timeout.
 */
extern int xylem_tls_send(
    xylem_tls_conn_t* tls,
    const void*       data,
    size_t            len);

/**
 * @brief Close and destroy a connection.
 *
 * @param tls  Connection handle.
 */
extern void xylem_tls_close(xylem_tls_conn_t* tls);

/**
 * @brief Close and destroy a listener.
 *
 * @param ln  Listener handle.
 */
extern void xylem_tls_close_listener(xylem_tls_listener_t* ln);

/**
 * @brief Get the last error code from the connection.
 *
 * @param tls  Connection handle.
 *
 * @return Error code, or XYLEM_ERR_NONE if no error.
 */
extern xylem_err_t xylem_tls_get_error(xylem_tls_conn_t* tls);

/**
 * @brief Get the remote address of the connection.
 *
 * @param tls       Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer (46 bytes recommended).
 * @param port      Receives the remote port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tls_remote_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port);

/**
 * @brief Get the local address of the connection.
 *
 * @param tls       Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer (46 bytes recommended).
 * @param port      Receives the local port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tls_local_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port);

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
extern int xylem_tls_listener_addr(
    xylem_tls_listener_t* ln,
    char*                 host,
    size_t                host_len,
    uint16_t*             port);

/**
 * @brief Get the negotiated ALPN protocol.
 *
 * @param tls  Connection handle.
 *
 * @return Protocol string, or NULL if none negotiated.
 */
extern const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls);
