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

typedef struct xylem_tls_conn_s   xylem_tls_conn_t;
typedef struct xylem_tls_ctx_s    xylem_tls_ctx_t;
typedef struct xylem_tls_server_s xylem_tls_server_t;

/* TLS connection options. */
typedef struct xylem_tls_opts_s {
    xylem_tcp_opts_t tcp;        /*< Underlying TCP options. */
    const char*      hostname;   /*< SNI hostname for server certificate selection and hostname verification. */
} xylem_tls_opts_t;

/* TLS event callback set. */
typedef struct xylem_tls_handler_s {
    void (*on_connect)(xylem_tls_conn_t* tls);             /*< TLS handshake completed (client). */
    void (*on_accept)(xylem_tls_server_t* server,
                      xylem_tls_conn_t* tls);               /*< TLS handshake completed (server). */
    void (*on_read)(xylem_tls_conn_t* tls,
                    void* data, size_t len);                 /*< Decrypted data received. */
    void (*on_write_done)(xylem_tls_conn_t* tls,
                          const void* data, size_t len,
                          int status);                       /*< Write finished: 0 = sent, -1 = not sent. */
    void (*on_timeout)(xylem_tls_conn_t* tls,
                       xylem_tcp_timeout_type_t type);       /*< Timeout from underlying TCP layer. */
    void (*on_close)(xylem_tls_conn_t* tls,
                     int err, const char* errmsg);           /*< Closed: 0 = normal, -1 = internal error, >0 = platform errno. */
    void (*on_heartbeat_miss)(xylem_tls_conn_t* tls);       /*< Heartbeat miss from underlying TCP layer. */
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
 * @brief Enable TLS key material logging for traffic analysis.
 *
 * Opens the specified file in append mode and registers an OpenSSL
 * keylog callback on the context. All TLS sessions using this context
 * will write key material in NSS Key Log Format, which Wireshark can
 * use to decrypt captured traffic.
 *
 * Passing NULL as path disables logging and closes the file.
 * Calling again with a new path closes the previous file first.
 *
 * @param ctx   Context handle.
 * @param path  File path to append key material to, or NULL to disable.
 *
 * @return 0 on success, -1 on failure (e.g. file open error).
 *
 * @note Do not enable in production. The exported key material can
 *       decrypt all TLS sessions using this context.
 */
extern int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path);

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
 * @param opts     TLS options, NULL for defaults.
 *
 * @return TLS connection handle, or NULL on failure.
 */
extern xylem_tls_conn_t* xylem_tls_dial(xylem_loop_t* loop,
                                   xylem_addr_t* addr,
                                   xylem_tls_ctx_t* ctx,
                                   xylem_tls_handler_t* handler,
                                   xylem_tls_opts_t* opts);

/**
 * @brief Send data over a TLS connection.
 *
 * Encrypts plaintext via SSL_write and sends the resulting
 * ciphertext over the underlying TCP connection. Returns
 * immediately; handler->on_write_done fires on completion.
 *
 * Thread-safe: may be called from any thread. When called from a
 * non-loop thread, the data is copied and posted to the loop thread
 * for asynchronous encryption and send. The caller must ensure the
 * connection has not been destroyed (i.e. on_close has not yet fired).
 *
 * @param tls   TLS connection handle.
 * @param data  Plaintext data to send.
 * @param len   Data length in bytes.
 *
 * @return 0 on success (enqueued), -1 on failure.
 */
extern int xylem_tls_send(xylem_tls_conn_t* tls,
                          const void* data, size_t len);

/**
 * @brief Close a TLS connection.
 *
 * Sends a TLS close_notify, then closes the underlying TCP
 * connection. handler->on_close fires when complete.
 *
 * Thread-safe: may be called from any thread. When called from a
 * non-loop thread, the close is posted to the loop thread. The
 * caller must ensure the connection has not been destroyed (i.e.
 * on_close has not yet fired, or the caller holds an acquire ref).
 *
 * @param tls  TLS connection handle.
 */
extern void xylem_tls_close(xylem_tls_conn_t* tls);

/**
 * @brief Acquire a reference to a TLS connection.
 *
 * Increments the connection's reference count, preventing the
 * underlying memory from being freed until a matching release
 * call. Must be called on the loop thread (typically in
 * on_connect or on_accept) before passing the connection handle
 * to another thread.
 *
 * @param tls  TLS connection handle.
 */
extern void xylem_tls_conn_acquire(xylem_tls_conn_t* tls);

/**
 * @brief Release a reference to a TLS connection.
 *
 * Decrements the reference count. When the count reaches zero,
 * the connection memory is freed. May be called from any thread.
 *
 * @param tls  TLS connection handle.
 */
extern void xylem_tls_conn_release(xylem_tls_conn_t* tls);

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
extern const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls);

/**
 * @brief Get the peer address of a TLS connection.
 *
 * Returns the peer address from the underlying TCP connection.
 * The pointer is valid for the lifetime of the TLS connection.
 *
 * @param tls  TLS connection handle.
 *
 * @return Peer address, or NULL if not available.
 */
extern const xylem_addr_t* xylem_tls_get_peer_addr(xylem_tls_conn_t* tls);

/**
 * @brief Get the event loop associated with a TLS connection.
 *
 * @param tls  TLS connection handle.
 *
 * @return Loop handle.
 */
extern xylem_loop_t* xylem_tls_get_loop(xylem_tls_conn_t* tls);

/**
 * @brief Get user data attached to a TLS connection.
 *
 * @param tls  TLS connection handle.
 *
 * @return User data pointer.
 */
extern void* xylem_tls_get_userdata(xylem_tls_conn_t* tls);

/**
 * @brief Set user data on a TLS connection.
 *
 * @param tls  TLS connection handle.
 * @param ud   User data pointer.
 */
extern void xylem_tls_set_userdata(xylem_tls_conn_t* tls, void* ud);

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
 * @param opts     TLS options, NULL for defaults.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_tls_server_t* xylem_tls_listen(xylem_loop_t* loop,
                                            xylem_addr_t* addr,
                                            xylem_tls_ctx_t* ctx,
                                            xylem_tls_handler_t* handler,
                                            xylem_tls_opts_t* opts);

/**
 * @brief Close a TLS server.
 *
 * Stops accepting new connections, closes all existing TLS
 * connections accepted by this server, and closes the underlying
 * TCP server.
 *
 * @param server  Server handle.
 */
extern void xylem_tls_close_server(xylem_tls_server_t* server);

/**
 * @brief Get user data attached to a TLS server.
 *
 * @param server  Server handle.
 *
 * @return User data pointer.
 */
extern void* xylem_tls_server_get_userdata(xylem_tls_server_t* server);

/**
 * @brief Set user data on a TLS server.
 *
 * @param server  Server handle.
 * @param ud      User data pointer.
 */
extern void xylem_tls_server_set_userdata(xylem_tls_server_t* server,
                                          void* ud);
