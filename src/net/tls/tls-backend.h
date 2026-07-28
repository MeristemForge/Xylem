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

/**
 * Backend-neutral TLS/DTLS engine interface.
 *
 * The engine (tls.c / dtls.c) owns sockets, iowait parking, locking,
 * refcounting, and the DTLS session machinery; it drives an SSL state
 * machine exclusively through this interface and includes no SSL-library
 * header. tls-backend-openssl.c implements every function below. Backend
 * selection is compile-time only; the current build supports OpenSSL.
 *
 * CONCURRENCY CONTRACT (precondition every backend may assume): the engine
 * holds a per-connection mutex across every conn op that touches the state
 * machine, exactly as it serializes the underlying library calls today.
 * Backends therefore use a plain, non-thread-safe state-machine object and
 * perform NO internal locking. Transport callbacks must be non-blocking:
 * they report EAGAIN through TLS_BACKEND_IO_AGAIN, and the engine owns
 * coroutine parking.
 */

typedef struct tls_backend_ctx_s  tls_backend_ctx_t;
typedef struct tls_backend_conn_s tls_backend_conn_t;

#define TLS_BACKEND_IO_AGAIN (-2)
#define TLS_BACKEND_IDENTITY_CAP 256

typedef int (*tls_backend_io_read_fn_t)(
    void* user,
    void* buf,
    int   len);

typedef int (*tls_backend_io_write_fn_t)(
    void*       user,
    const void* buf,
    int         len);

typedef struct tls_backend_io_s {
    void*                     user;
    tls_backend_io_read_fn_t  read;
    tls_backend_io_write_fn_t write;
} tls_backend_io_t;

/* Result of a handshake/read/write step. */
typedef enum {
    TLS_BACKEND_OK,
    TLS_BACKEND_WANT_READ,
    TLS_BACKEND_WANT_WRITE,
    TLS_BACKEND_CLOSED,   /* clean peer shutdown */
    TLS_BACKEND_ERROR
} tls_backend_state_t;

/* Verify policy, computed by the engine from role + ctx intent. */
typedef enum {
    TLS_BACKEND_VERIFY_NONE,
    TLS_BACKEND_VERIFY_PEER,     /* verify chain; peer cert optional (client) */
    TLS_BACKEND_VERIFY_REQUIRE   /* verify chain; peer cert required (mTLS)   */
} tls_backend_verify_t;

typedef enum {
    TLS_BACKEND_PROTO_TLS,
    TLS_BACKEND_PROTO_DTLS
} tls_backend_proto_t;

typedef enum {
    TLS_BACKEND_IDENTITY_NONE,
    TLS_BACKEND_IDENTITY_DNS,
    TLS_BACKEND_IDENTITY_IP
} tls_backend_identity_t;

/* One-shot pre-handshake connection configuration owned by the engine. */
typedef struct {
    tls_backend_verify_t   verify;
    tls_backend_identity_t identity_type;
    char                   identity[TLS_BACKEND_IDENTITY_CAP];
} tls_backend_handshake_cfg_t;

/**
 * @brief Create a backend TLS/DTLS context.
 *
 * @param proto  TLS_BACKEND_PROTO_TLS or TLS_BACKEND_PROTO_DTLS.
 *
 * @return Context handle, or NULL on failure.
 */
extern tls_backend_ctx_t* tls_backend_ctx_create(tls_backend_proto_t proto);

/**
 * @brief Destroy a backend context. NULL-safe.
 *
 * Backend listeners and connections borrow the context and do not retain a
 * reference. The context must outlive every object created from it and all
 * operations on those objects. Complete configuration before creating those
 * objects and do not modify it after handing it to the engine.
 *
 * @param ctx  Context handle.
 */
extern void               tls_backend_ctx_destroy(tls_backend_ctx_t* ctx);

/**
 * @brief Load a PEM certificate chain and private key from files.
 *
 * When hostname is NULL the identity becomes the ctx default; otherwise
 * it is registered for SNI selection on that hostname.
 *
 * @param ctx        Context handle.
 * @param hostname   SNI hostname, or NULL for the default identity.
 * @param cert_file  Path to the PEM certificate chain (leaf first).
 * @param key_file   Path to the PEM private key.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_backend_ctx_load_cert_file(
    tls_backend_ctx_t* ctx,
    const char*        hostname,
    const char*        cert_file,
    const char*        key_file);

/**
 * @brief Load a PEM certificate chain and private key from memory.
 *
 * Same semantics as tls_backend_ctx_load_cert_file but reads from
 * in-memory buffers, which are not retained after the call returns.
 *
 * @param ctx       Context handle.
 * @param hostname  SNI hostname, or NULL for the default identity.
 * @param cert_pem  PEM certificate chain bytes (leaf first).
 * @param cert_len  Length of cert_pem in bytes.
 * @param key_pem   PEM private key bytes.
 * @param key_len   Length of key_pem in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_backend_ctx_load_cert_mem(
    tls_backend_ctx_t* ctx,
    const char*        hostname,
    const void*        cert_pem,
    size_t             cert_len,
    const void*        key_pem,
    size_t             key_len);

/**
 * @brief Add a CA certificate file to the trust store.
 *
 * @param ctx      Context handle.
 * @param ca_file  Path to a PEM CA certificate (or bundle) file.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_backend_ctx_load_ca_file(
    tls_backend_ctx_t* ctx,
    const char*        ca_file);

/**
 * @brief Load public-CA trust anchors: the system store plus a fallback.
 *
 * Loads additively from the platform system store and, when non-NULL,
 * fallback_ca_file. Succeeds if either source loads.
 *
 * @param ctx               Context handle.
 * @param fallback_ca_file  PEM CA bundle path, or NULL for none.
 *
 * @return 0 if at least one source loaded, -1 if none did.
 */
extern int tls_backend_ctx_load_system_ca(
    tls_backend_ctx_t* ctx,
    const char*        fallback_ca_file);

/**
 * @brief Set the ALPN protocol list (offered by clients, selected by
 *        servers).
 *
 * @param ctx        Context handle.
 * @param protocols  Array of non-empty protocol strings, each at most
 *                   255 bytes (e.g. "h2", "http/1.1").
 * @param count      Number of protocols.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_backend_ctx_set_alpn(
    tls_backend_ctx_t* ctx,
    const char**       protocols,
    size_t             count);

/**
 * @brief Enable NSS key-log output for Wireshark decryption.
 *
 * @param ctx   Context handle.
 * @param path  Output file path, or NULL to disable.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_backend_ctx_set_keylog(
    tls_backend_ctx_t* ctx,
    const char*        path);

/**
 * @brief Create a connection state machine bound to a context.
 *
 * @param ctx        Context handle.
 * @param is_server  true for the accept (server) role, false for connect.
 * @param io         Non-blocking transport I/O callbacks.
 *
 * @return Connection handle, or NULL on failure.
 */
extern tls_backend_conn_t* tls_backend_conn_create(
    tls_backend_ctx_t* ctx,
    bool               is_server,
    const tls_backend_io_t* io);

/**
 * @brief Destroy a connection state machine. NULL-safe.
 *
 * @param c  Connection handle.
 */
extern void tls_backend_conn_destroy(tls_backend_conn_t* c);

/**
 * @brief Apply one-shot pre-handshake configuration.
 *
 * The backend copies any string it retains; cfg may reference engine
 * temporaries.
 *
 * @param c    Connection handle.
 * @param cfg  Handshake configuration.
 *
 * @return 0 on success, -1 when SNI or identity setup fails.
 */
extern int tls_backend_conn_configure(
    tls_backend_conn_t*                c,
    const tls_backend_handshake_cfg_t* cfg);

/**
 * @brief Advance the handshake state machine one step.
 *
 * @param c  Connection handle.
 *
 * @return TLS_BACKEND_OK once complete, WANT_READ/WANT_WRITE to pump,
 *         or TLS_BACKEND_ERROR on failure.
 */
extern tls_backend_state_t tls_backend_conn_handshake(tls_backend_conn_t* c);

/**
 * @brief Read decrypted application data.
 *
 * @param c      Connection handle.
 * @param buf    Destination buffer.
 * @param len    Capacity of buf in bytes.
 * @param out_n  Receives the number of plaintext bytes produced.
 *
 * @return TLS_BACKEND_OK with *out_n set, WANT_READ/WANT_WRITE to pump,
 *         TLS_BACKEND_CLOSED on clean shutdown, or TLS_BACKEND_ERROR.
 */
extern tls_backend_state_t tls_backend_conn_read(
    tls_backend_conn_t* c,
    void*               buf,
    int                 len,
    int*                out_n);

/**
 * @brief Write application data for encryption.
 *
 * @param c      Connection handle.
 * @param buf    Source plaintext buffer.
 * @param len    Number of bytes to write.
 * @param out_n  Receives the number of plaintext bytes accepted.
 *
 * @return TLS_BACKEND_OK with *out_n set, WANT_READ/WANT_WRITE to pump,
 *         or TLS_BACKEND_ERROR.
 */
extern tls_backend_state_t tls_backend_conn_write(
    tls_backend_conn_t* c,
    const void*         buf,
    int                 len,
    int*                out_n);

/**
 * @brief Send a best-effort close-notify through the transport BIO.
 *
 * @param c  Connection handle.
 */
extern void tls_backend_conn_shutdown(tls_backend_conn_t* c);

/**
 * @brief Copy the negotiated ALPN protocol into a buffer.
 *
 * Writes an empty string when no protocol was negotiated.
 *
 * @param c    Connection handle.
 * @param buf  Destination buffer.
 * @param cap  Capacity of buf in bytes.
 */
extern void tls_backend_conn_get_alpn(
    tls_backend_conn_t* c,
    char*               buf,
    size_t              cap);

/**
 * @brief Set the link MTU so records fit a single datagram.
 *
 * @param c    Connection handle.
 * @param mtu  Link MTU in bytes; 0 leaves the backend default.
 */
extern void dtls_backend_conn_set_mtu(tls_backend_conn_t* c, uint16_t mtu);

/**
 * @brief Bind the peer address used for DTLS cookie generation.
 *
 * @param c         Connection handle.
 * @param sockaddr  Peer sockaddr bytes.
 * @param salen     Length of the sockaddr in bytes.
 */
extern void dtls_backend_conn_set_peer_addr(
    tls_backend_conn_t* c,
    const void*         sockaddr,
    size_t              salen);

/**
 * @brief Query the current DTLS retransmit timeout.
 *
 * @param c       Connection handle.
 * @param out_ms  Receives the timeout in ms (at least 1) when pending.
 *
 * @return true if a timeout is pending, false otherwise.
 */
extern bool dtls_backend_conn_get_timeout(
    tls_backend_conn_t* c,
    uint64_t*           out_ms);

/**
 * @brief Process a DTLS retransmit-timer expiry (retransmits a flight).
 *
 * @param c  Connection handle.
 */
extern void dtls_backend_conn_handle_timeout(tls_backend_conn_t* c);
