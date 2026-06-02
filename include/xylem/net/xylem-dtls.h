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
    /**
     * Timeout in milliseconds for completing the DTLS handshake.
     *
     * - Dial: covers the handshake (with retransmissions).
     * - Accept: per-session handshake timeout on the server side.
     *
     * 0 means default (30s).
     */
    uint64_t    handshake_timeout_ms;
    /**
     * Expected peer identity. Accepts a DNS hostname or numeric IP
     * literal (IPv4 or IPv6). Drives SNI (only sent for DNS names;
     * RFC 6066) and certificate identity verification.
     */
    const char* server_name;
    /**
     * Link-layer MTU hint passed to OpenSSL (DTLS_set_link_mtu) to
     * bound the size of DTLS handshake/record datagrams so they are
     * not IP-fragmented. Applies to both dial and accept.
     *
     * This stack drives OpenSSL through memory BIOs, so OpenSSL cannot
     * discover the path MTU on its own. With 0, OpenSSL falls back to
     * a small conservative MTU (handshake still works, just more
     * fragments); set this to your link MTU (e.g. 1500) for efficient
     * handshakes without IP fragmentation.
     */
    uint16_t    mtu;
} xylem_dtls_opts_t;

/**
 * @brief Create a DTLS context.
 *
 * Defaults: as a client, the server certificate is verified; as a
 * server, no client certificate is requested; DTLS 1.2 minimum; cookie
 * exchange enabled for server-side anti-amplification. See
 * xylem_dtls_ctx_verify_server / verify_client to change the
 * verification defaults.
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
extern int xylem_dtls_ctx_load_cert(xylem_dtls_ctx_t* ctx,
                                    const char* hostname,
                                    const char* cert, const char* key);

/**
 * @brief Load a PEM certificate chain and private key from memory.
 *
 * Same semantics as xylem_dtls_ctx_load_cert but reads the PEM data
 * from in-memory buffers instead of files, for certificates sourced
 * from a secret store, environment, or embedded resource. The buffers
 * are not retained after this call returns.
 *
 * @param ctx       Context handle.
 * @param hostname  Domain name for SNI selection, or NULL for default.
 * @param cert_pem  PEM certificate chain bytes (leaf first).
 * @param cert_len  Length of cert_pem in bytes.
 * @param key_pem   PEM private key bytes.
 * @param key_len   Length of key_pem in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_ctx_load_cert_mem(xylem_dtls_ctx_t* ctx,
                                        const char* hostname,
                                        const void* cert_pem,
                                        size_t      cert_len,
                                        const void* key_pem,
                                        size_t      key_len);

/**
 * @brief Add a CA certificate file to the trust store.
 *
 * The CAs become trust anchors for verifying the peer certificate (the
 * server on a client, or the client on an mTLS server). Only these CAs
 * are trusted unless xylem_dtls_ctx_load_system_ca is also called,
 * which is the narrow trust wanted for a private PKI or mTLS.
 *
 * @param ctx      Context handle.
 * @param ca_file  Path to a PEM CA certificate (or bundle) file.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_ctx_load_ca(xylem_dtls_ctx_t* ctx,
                                  const char* ca_file);

/**
 * @brief Trust the system root certificate store.
 *
 * Lets a client verify servers using certificates from public CAs
 * without naming a CA file. Combine with xylem_dtls_ctx_load_ca to
 * also trust a private CA. Avoid on an mTLS server, where it would
 * accept any client certificate chaining to a public CA.
 *
 * @param ctx  Context handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_ctx_load_system_ca(xylem_dtls_ctx_t* ctx);

/**
 * @brief Set whether a client verifies the server certificate.
 *
 * Applies to the client role (xylem_dtls_dial). When enabled (the
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
extern void xylem_dtls_ctx_verify_server(xylem_dtls_ctx_t* ctx, bool enable);

/**
 * @brief Set whether a server requires a client certificate (mTLS).
 *
 * Applies to the server role (xylem_dtls_listen). When enabled the
 * server requests a client certificate during the handshake and fails
 * the connection if the client does not present one that verifies
 * against the configured CA (see xylem_dtls_ctx_load_ca). Disabled by
 * default, so a plain server accepts clients without a certificate.
 *
 * Has no effect on the client role. A context may be reused as both
 * client and server; this setting only changes server accepts.
 *
 * @param ctx     Context handle.
 * @param enable  true to require and verify a client cert, false to
 *                request none (default).
 */
extern void xylem_dtls_ctx_verify_client(xylem_dtls_ctx_t* ctx, bool enable);

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
 * Suspends the calling coroutine until the handshake completes or
 * handshake_timeout_ms elapses (default 30s).
 *
 * The host parameter is the network destination; it is not used for
 * certificate verification. Set opts->server_name to verify identity.
 *
 * @param host  Remote hostname or IP address.
 * @param port  Remote port.
 * @param ctx   DTLS context.
 * @param opts  Options, or NULL for defaults.
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
 * @param host  Bind address (e.g. "0.0.0.0"), or NULL for any.
 * @param port  Bind port.
 * @param ctx   DTLS context with cert+key loaded.
 * @param opts  Options, or NULL for defaults.
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
 * Suspends until a client completes the handshake.
 *
 * @param ln  Listener handle.
 *
 * @return Accepted connection, or NULL if the listener is closed.
 */
extern xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln);

/**
 * @brief Read a datagram from the connection.
 *
 * Suspends until data arrives, the read deadline passes, or
 * the connection is closed.
 *
 * @param dtls  Connection handle.
 * @param buf   Destination buffer.
 * @param len   Buffer size.
 *
 * @return Bytes received (>0), 0 on peer close, -1 on error.
 */
extern int xylem_dtls_read(
    xylem_dtls_conn_t* dtls,
    void*              buf,
    int                len);

/**
 * @brief Write a datagram to the connection.
 *
 * @param dtls  Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to send.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_dtls_write(
    xylem_dtls_conn_t* dtls,
    const void*        data,
    int                len);

/**
 * @brief Set the read deadline for the connection.
 *
 * Once the clock passes the deadline, in-flight and subsequent
 * xylem_dtls_read() calls return -1.
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
 * xylem_dtls_write() calls return -1.
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
