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

typedef struct xylem_tls_conn_s     xylem_tls_conn_t;
typedef struct xylem_tls_ctx_s      xylem_tls_ctx_t;
typedef struct xylem_tls_listener_s xylem_tls_listener_t;

typedef struct xylem_tls_opts_s {
    bool disable_mss_clamp; /*< Disable MSS clamping on the socket. */
    /**
     * Timeout in milliseconds for completing the TLS handshake.
     *
     * - Dial: covers the full TCP connect + TLS handshake.
     * - Accept: covers the TLS handshake on an accepted TCP connection.
     *
     * 0 means no timeout. Setting a non-zero value is strongly
     * recommended on the accept side; without it, slow or malicious
     * clients can park the accept coroutine indefinitely (slowloris).
     */
    uint64_t handshake_timeout_ms;
    /**
     * Expected peer identity. Accepts a DNS hostname (e.g. "bank.com")
     * or a numeric IP literal (IPv4 or IPv6). Drives two things:
     *   - SNI extension (only sent for DNS names; RFC 6066 forbids
     *     sending IP literals as SNI).
     *   - Certificate identity verification: matched against the
     *     peer certificate's DNS-type or IP-type SAN entries.
     *
     * Required for secure clients when verify_peer is enabled. If
     * NULL, only certificate chain trust is checked, not the peer's
     * identity, so any cert signed by a trusted CA is accepted
     * (MITM risk).
     */
    const char* server_name;
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
 * When hostname is NULL the certificate becomes the default (used when
 * no SNI matches). When hostname is non-NULL, the certificate is bound
 * to that domain and selected via SNI during handshake.
 *
 * @param ctx       Context handle.
 * @param hostname  Domain name for SNI selection, or NULL for default.
 * @param cert      Path to PEM certificate chain file.
 * @param key       Path to PEM private key file.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_ctx_load_cert(
    xylem_tls_ctx_t* ctx,
    const char*      hostname,
    const char*      cert,
    const char*      key);

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
 * @brief Set whether a client verifies the server certificate.
 *
 * Applies to the client role (xylem_tls_dial). When enabled (the
 * default) the server certificate chain is validated and, if
 * opts.server_name is set, its identity is checked. Disabling it makes
 * the client accept any certificate, which exposes it to MITM attacks;
 * use only for tests or trusted networks.
 *
 * Has no effect on the server role. A context may be reused as both
 * client and server; this setting only changes client dials.
 *
 * @param ctx     Context handle.
 * @param enable  true to verify the server (default), false to skip.
 */
extern void xylem_tls_ctx_verify_server(xylem_tls_ctx_t* ctx, bool enable);

/**
 * @brief Set whether a server requires a client certificate (mTLS).
 *
 * Applies to the server role (xylem_tls_listen). When enabled the
 * server requests a client certificate during the handshake and fails
 * the connection if the client does not present one that verifies
 * against the configured CA (see xylem_tls_ctx_set_ca). Disabled by
 * default, so a plain server accepts clients without a certificate.
 *
 * Has no effect on the client role. A context may be reused as both
 * client and server; this setting only changes server accepts.
 *
 * @param ctx     Context handle.
 * @param enable  true to require and verify a client cert, false to
 *                request none (default).
 */
extern void xylem_tls_ctx_verify_client(xylem_tls_ctx_t* ctx, bool enable);

/**
 * @brief Set the ALPN protocol list.
 *
 * @param ctx        Context handle.
 * @param protocols  Array of protocol strings (e.g. "h2", "http/1.1").
 * @param count      Number of protocols.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_ctx_set_alpn(
    xylem_tls_ctx_t* ctx,
    const char**     protocols,
    size_t           count);

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
 * and the TLS handshake completes, or handshake_timeout_ms elapses.
 *
 * The host parameter is the network destination to connect to; it is
 * not used for certificate verification. To verify the peer's
 * identity, set opts->server_name. These can differ -- e.g. dialing
 * a load balancer IP while expecting a certificate for the backend
 * service hostname.
 *
 * @param host  Remote hostname or IP address to connect to.
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
 * Per-connection handshake failures (bad cert, protocol mismatch,
 * client abort, handshake timeout) are handled internally: the
 * offending connection is dropped and accept keeps waiting for the
 * next client. This is required so a single bad client cannot tear
 * down a server's accept loop. NULL is therefore returned only when
 * the listener is closed (or on a fatal resource shortage that
 * prevents wrapping an accepted socket), so callers can treat NULL
 * as "stop accepting".
 *
 * @param ln  Listener handle.
 *
 * @return Accepted connection, or NULL when the listener is closed.
 */
extern xylem_tls_conn_t* xylem_tls_accept(xylem_tls_listener_t* ln);

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
 * @brief Read data from the connection (read-some semantics).
 *
 * Returns available plaintext data. Suspends the calling coroutine
 * if no data is immediately available. At most len bytes are
 * returned; the actual count may be less.
 *
 * @param tls  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 *
 * @return Bytes read (>0), 0 on peer close, -1 on error/timeout.
 */
extern int xylem_tls_read(
    xylem_tls_conn_t* tls,
    void*             buf,
    int               len);

/**
 * @brief Write all data to the connection.
 *
 * Encrypts and loops internally until all len bytes are sent or
 * an error occurs. Suspends the calling coroutine as needed.
 *
 * @param tls   Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to write.
 *
 * @return 0 on success, -1 on error or timeout.
 */
extern int xylem_tls_write(
    xylem_tls_conn_t* tls,
    const void*       data,
    int               len);

/**
 * @brief Close a connection. Idempotent.
 *
 * Wakes any coroutine blocked in read/write. Read any needed state
 * (xylem_tls_remote_addr) before closing.
 *
 * @param tls  Connection handle.
 */
extern void xylem_tls_close(xylem_tls_conn_t* tls);

/**
 * @brief Close and destroy a listener. Idempotent.
 *
 * Wakes any coroutine blocked in xylem_tls_accept().
 *
 * @param ln  Listener handle.
 */
extern void xylem_tls_close_listener(xylem_tls_listener_t* ln);

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
