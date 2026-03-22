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

#include "xylem/xylem-addr.h"
#include "xylem/xylem-loop.h"
#include "xylem/xylem-tcp.h"

typedef struct xylem_tls_s        xylem_tls_t;
typedef struct xylem_tls_ctx_s    xylem_tls_ctx_t;
typedef struct xylem_tls_server_s xylem_tls_server_t;

typedef struct xylem_tls_handler_s {
    void (*on_connect)(xylem_tls_t* tls);
    void (*on_accept)(xylem_tls_t* tls);
    void (*on_read)(xylem_tls_t* tls, void* data, size_t len);
    void (*on_write_done)(xylem_tls_t* tls,
                          void* data, size_t len, int status);
    void (*on_timeout)(xylem_tls_t* tls,
                       xylem_tcp_timeout_type_t type);
    void (*on_close)(xylem_tls_t* tls, int err);
    void (*on_heartbeat_miss)(xylem_tls_t* tls);
} xylem_tls_handler_t;

/**
 * @brief Create a TLS context.
 *
 * Allocates a reusable TLS context wrapping an OpenSSL SSL_CTX.
 * One context can be shared across multiple connections and servers.
 *
 * @return Context handle, or NULL on failure.
 */
extern xylem_tls_ctx_t* xylem_tls_ctx_create(void);

/**
 * @brief Destroy a TLS context.
 *
 * Frees the SSL_CTX and the context struct. Must not be called
 * while connections using this context are still active.
 *
 * @param ctx  Context handle.
 */
extern void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx);

/**
 * @brief Load certificate chain and private key.
 *
 * Loads a PEM-encoded certificate chain and private key from files.
 * Required for server contexts, optional for client contexts
 * (client certificate authentication).
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
 * @brief Set CA certificate for peer verification.
 *
 * @param ctx      Context handle.
 * @param ca_file  Path to CA certificate file (PEM format).
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_file);

/**
 * @brief Enable or disable peer certificate verification.
 *
 * When enabled, the handshake fails if the peer certificate
 * cannot be verified against the configured CA.
 *
 * @param ctx     Context handle.
 * @param enable  true to enable verification, false to disable.
 */
extern void xylem_tls_ctx_set_verify(xylem_tls_ctx_t* ctx, bool enable);

/**
 * @brief Set ALPN protocol list for negotiation.
 *
 * Configures the list of application-layer protocols offered
 * during the TLS handshake. For clients, these are proposed
 * protocols. For servers, these are accepted protocols.
 *
 * @param ctx        Context handle.
 * @param protocols  Array of null-terminated protocol strings.
 * @param count      Number of protocols in the array.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_ctx_set_alpn(xylem_tls_ctx_t* ctx,
                                  const char** protocols, size_t count);

/**
 * @brief Initiate an asynchronous TLS connection.
 *
 * Creates a TCP connection to the target address, then performs
 * a TLS handshake. handler->on_connect fires only after the
 * handshake completes successfully.
 *
 * @param loop     Event loop.
 * @param addr     Target address.
 * @param ctx      TLS context.
 * @param handler  Event callback set.
 * @param opts     TCP options, NULL for defaults.
 *
 * @return TLS connection handle, or NULL on failure.
 */
extern xylem_tls_t* xylem_tls_dial(xylem_loop_t* loop,
                                   xylem_addr_t* addr,
                                   xylem_tls_ctx_t* ctx,
                                   xylem_tls_handler_t* handler,
                                   xylem_tcp_opts_t* opts);

/**
 * @brief Send data over a TLS connection.
 *
 * Encrypts plaintext via SSL_write and sends the resulting
 * ciphertext over the underlying TCP connection. Returns
 * immediately; handler->on_write_done fires on completion.
 *
 * @param tls   TLS connection handle.
 * @param data  Plaintext data to send.
 * @param len   Data length in bytes.
 *
 * @return 0 on success (enqueued), -1 on failure.
 */
extern int xylem_tls_send(xylem_tls_t* tls,
                          const void* data, size_t len);

/**
 * @brief Close a TLS connection.
 *
 * Sends a TLS close_notify, then closes the underlying TCP
 * connection. handler->on_close fires when complete.
 *
 * @param tls  TLS connection handle.
 */
extern void xylem_tls_close(xylem_tls_t* tls);

/**
 * @brief Set SNI hostname for the connection.
 *
 * Must be called before xylem_tls_dial. Sets the Server Name
 * Indication extension and enables hostname verification.
 *
 * @param tls       TLS connection handle.
 * @param hostname  Server hostname string.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_set_hostname(xylem_tls_t* tls, const char* hostname);

/**
 * @brief Get the negotiated ALPN protocol.
 *
 * Returns the protocol selected during the TLS handshake,
 * or NULL if no protocol was negotiated.
 *
 * @param tls  TLS connection handle.
 *
 * @return Null-terminated protocol string, or NULL.
 */
extern const char* xylem_tls_get_alpn(xylem_tls_t* tls);

/**
 * @brief Get user data attached to a TLS connection.
 *
 * @param tls  TLS connection handle.
 *
 * @return User data pointer.
 */
extern void* xylem_tls_get_userdata(xylem_tls_t* tls);

/**
 * @brief Set user data on a TLS connection.
 *
 * @param tls  TLS connection handle.
 * @param ud   User data pointer.
 */
extern void xylem_tls_set_userdata(xylem_tls_t* tls, void* ud);

/**
 * @brief Create a TLS server and start listening.
 *
 * Binds to the specified address and accepts incoming TCP
 * connections, performing a TLS handshake on each one.
 * handler->on_accept fires after each successful handshake.
 * The context must have a certificate and key loaded.
 *
 * @param loop     Event loop.
 * @param addr     Bind address.
 * @param ctx      TLS context with cert+key loaded.
 * @param handler  Event callback set.
 * @param opts     TCP options, NULL for defaults.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_tls_server_t* xylem_tls_listen(xylem_loop_t* loop,
                                            xylem_addr_t* addr,
                                            xylem_tls_ctx_t* ctx,
                                            xylem_tls_handler_t* handler,
                                            xylem_tcp_opts_t* opts);

/**
 * @brief Close a TLS server.
 *
 * Stops accepting new connections and closes the underlying
 * TCP server. Existing TLS connections are not affected and
 * must be closed individually.
 *
 * @param server  Server handle.
 */
extern void xylem_tls_close_server(xylem_tls_server_t* server);
