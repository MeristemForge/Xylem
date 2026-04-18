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

/* Opaque DTLS session handle. */
typedef struct xylem_dtls_s        xylem_dtls_t;
/* Opaque DTLS context handle. */
typedef struct xylem_dtls_ctx_s    xylem_dtls_ctx_t;
/* Opaque DTLS server handle. */
typedef struct xylem_dtls_server_s xylem_dtls_server_t;

/* DTLS event callback set. */
typedef struct xylem_dtls_handler_s {
    void (*on_connect)(xylem_dtls_t* dtls);                /*< DTLS handshake completed (client). */
    void (*on_accept)(xylem_dtls_server_t* server,
                      xylem_dtls_t* dtls);                  /*< DTLS handshake completed (server). */
    void (*on_read)(xylem_dtls_t* dtls,
                    void* data, size_t len);                 /*< Decrypted datagram received. */
    void (*on_close)(xylem_dtls_t* dtls,
                     int err, const char* errmsg);           /*< Closed: 0 = normal, -1 = internal error, >0 = platform errno. */
} xylem_dtls_handler_t;

/**
 * @brief Create a DTLS context.
 *
 * Allocates a reusable DTLS context wrapping an OpenSSL SSL_CTX
 * with DTLS_method(). Automatically configures cookie generation
 * and verification callbacks for server use.
 *
 * @return Context handle, or NULL on failure.
 */
extern xylem_dtls_ctx_t* xylem_dtls_ctx_create(void);

/**
 * @brief Destroy a DTLS context.
 *
 * Frees the SSL_CTX and the context struct.
 *
 * @param ctx  Context handle.
 */
extern void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx);

/**
 * @brief Load certificate chain and private key.
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
 * @brief Set CA certificate for peer verification.
 *
 * @param ctx      Context handle.
 * @param ca_file  Path to CA certificate file (PEM format).
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_ctx_set_ca(xylem_dtls_ctx_t* ctx,
                                 const char* ca_file);

/**
 * @brief Enable or disable peer certificate verification.
 *
 * @param ctx     Context handle.
 * @param enable  true to enable, false to disable.
 */
extern void xylem_dtls_ctx_set_verify(xylem_dtls_ctx_t* ctx, bool enable);

/**
 * @brief Set ALPN protocol list for negotiation.
 *
 * @param ctx        Context handle.
 * @param protocols  Array of null-terminated protocol strings.
 * @param count      Number of protocols.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_ctx_set_alpn(xylem_dtls_ctx_t* ctx,
                                   const char** protocols, size_t count);

/**
 * @brief Enable DTLS key material logging for traffic analysis.
 *
 * Opens the specified file in append mode and registers an OpenSSL
 * keylog callback on the context. All DTLS sessions using this context
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
 *       decrypt all DTLS sessions using this context.
 */
extern int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx,
                                     const char* path);

/**
 * @brief Initiate an asynchronous DTLS connection.
 *
 * Creates a connected UDP socket to the target address, then
 * performs a DTLS handshake. handler->on_connect fires after
 * the handshake completes successfully.
 *
 * @param loop     Event loop.
 * @param addr     Target address.
 * @param ctx      DTLS context.
 * @param handler  Event callback set.
 *
 * @return DTLS session handle, or NULL on failure.
 */
extern xylem_dtls_t* xylem_dtls_dial(xylem_loop_t* loop,
                                     xylem_addr_t* addr,
                                     xylem_dtls_ctx_t* ctx,
                                     xylem_dtls_handler_t* handler);

/**
 * @brief Send a datagram over a DTLS session.
 *
 * Encrypts plaintext via SSL_write and sends the resulting
 * ciphertext over UDP.
 *
 * @param dtls  DTLS session handle.
 * @param data  Plaintext data to send.
 * @param len   Data length in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_send(xylem_dtls_t* dtls,
                           const void* data, size_t len);

/**
 * @brief Close a DTLS session.
 *
 * Sends close_notify and closes the underlying UDP socket.
 * handler->on_close fires with err=0 and errmsg=NULL for a normal
 * close, or with an SSL error code and description when the close
 * is triggered by an SSL error.
 *
 * @param dtls  DTLS session handle.
 */
extern void xylem_dtls_close(xylem_dtls_t* dtls);

/**
 * @brief Get the negotiated ALPN protocol.
 *
 * @param dtls  DTLS session handle.
 *
 * @return Null-terminated protocol string, or NULL.
 */
extern const char* xylem_dtls_get_alpn(xylem_dtls_t* dtls);

/**
 * @brief Get the peer address of a DTLS session.
 *
 * @param dtls  DTLS session handle.
 *
 * @return Peer address, or NULL if not available.
 */
extern const xylem_addr_t* xylem_dtls_get_peer_addr(xylem_dtls_t* dtls);

/**
 * @brief Get the event loop associated with a DTLS session.
 *
 * @param dtls  DTLS session handle.
 *
 * @return Loop handle.
 */
extern xylem_loop_t* xylem_dtls_get_loop(xylem_dtls_t* dtls);

/**
 * @brief Get user data attached to a DTLS session.
 *
 * @param dtls  DTLS session handle.
 *
 * @return User data pointer.
 */
extern void* xylem_dtls_get_userdata(xylem_dtls_t* dtls);

/**
 * @brief Set user data on a DTLS session.
 *
 * @param dtls  DTLS session handle.
 * @param ud    User data pointer.
 */
extern void xylem_dtls_set_userdata(xylem_dtls_t* dtls, void* ud);

/**
 * @brief Create a DTLS server and start listening.
 *
 * Binds a UDP socket and handles incoming DTLS handshakes.
 * Multiple sessions are demuxed by peer address on a single
 * UDP socket. handler->on_accept fires per successful handshake.
 *
 * @param loop     Event loop.
 * @param addr     Bind address.
 * @param ctx      DTLS context with cert+key loaded.
 * @param handler  Event callback set.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_dtls_server_t* xylem_dtls_listen(xylem_loop_t* loop,
                                              xylem_addr_t* addr,
                                              xylem_dtls_ctx_t* ctx,
                                              xylem_dtls_handler_t* handler);

/**
 * @brief Close a DTLS server.
 *
 * Closes all active sessions and the underlying UDP socket.
 *
 * @param server  Server handle.
 */
extern void xylem_dtls_close_server(xylem_dtls_server_t* server);

/**
 * @brief Get user data attached to a DTLS server.
 *
 * @param server  Server handle.
 *
 * @return User data pointer.
 */
extern void* xylem_dtls_server_get_userdata(xylem_dtls_server_t* server);

/**
 * @brief Set user data on a DTLS server.
 *
 * @param server  Server handle.
 * @param ud      User data pointer.
 */
extern void xylem_dtls_server_set_userdata(xylem_dtls_server_t* server,
                                           void* ud);
