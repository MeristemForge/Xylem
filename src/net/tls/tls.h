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

/**
 * Internal TLS engine.
 *
 * Defines the concrete connection/context/listener structs and the
 * engine API (tls_*), built on the backend TLS interface and the
 * coroutine runtime. The public xylem_tls_* surface is a thin shim over
 * these; internal consumers that need the engine directly -- e.g. the
 * HTTPS transport running the client handshake over a proxy-tunnel fd
 * via tls_client_handshake_fd -- include this header instead.
 *
 * Not part of the public API. Do not include outside the TLS/HTTP/WS
 * modules.
 */

_Pragma("once")

#include "xylem/net/xylem-tls.h"
#include "xylem/sync/xylem-mutex.h"

#include "net/tcp/stream.h"
#include "net/tls/tls-backend.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * The public header declares opaque xylem_tls_* handles; the engine
 * works on these concrete tls_* structs, which the public shim wraps by
 * first-member equivalence so the two layers never share a struct tag.
 * xylem_tls_opts_t has no internal alias: it is a transparent value type
 * the engine uses as-is.
 */
typedef struct tls_ctx_s      tls_ctx_t;
typedef struct tls_conn_s     tls_conn_t;
typedef struct tls_listener_s tls_listener_t;

struct tls_ctx_s {
    tls_backend_ctx_t* be;
    bool               verify_server;
    bool               verify_client;
};

struct tls_conn_s {
    tls_backend_conn_t* be;
    char*               rbuf;       /* recv staging, one TLS record */
    char*               wbuf;       /* drain staging, one TLS record */
    xylem_mutex_t*      ssl_mu;
    xylem_mutex_t*      rd_mu;
    xylem_mutex_t*      wr_mu;
    xylem_mutex_t*      hs_mu;          /* elects one lazy-handshake driver */
    stream_t*           stream;
    tls_ctx_t*          ctx;
    char                alpn[32];
    uint64_t            hs_timeout_ms;  /* copied from ln->opts at accept */
    _Atomic int         hs_state;       /* HS_DONE / HS_PENDING / HS_FAILED */
    _Atomic int32_t     refcnt;
    _Atomic bool        closed;
};

struct tls_listener_s {
    listener_t*      listener;
    tls_ctx_t*       ctx;
    xylem_tls_opts_t opts;
    _Atomic int32_t  refcnt;
    _Atomic bool     closed;
};

/**
 * @brief Create a TLS engine context.
 *
 * @return Context handle, or NULL on failure.
 */
extern tls_ctx_t* tls_ctx_create(void);

/**
 * @brief Destroy a TLS engine context. NULL-safe.
 *
 * @param ctx  Context handle.
 */
extern void       tls_ctx_destroy(tls_ctx_t* ctx);

/**
 * @brief Enable NSS key-log output for Wireshark decryption.
 *
 * @param ctx   Context handle.
 * @param path  Output file path, or NULL to disable.
 *
 * @return 0 on success, -1 on failure.
 */
extern int        tls_ctx_set_keylog(tls_ctx_t* ctx, const char* path);

/**
 * @brief Load a PEM certificate chain and private key from files.
 *
 * When hostname is NULL the identity becomes the ctx default; otherwise
 * it is registered for SNI selection on that hostname.
 *
 * @param ctx       Context handle.
 * @param hostname  SNI hostname, or NULL for the default identity.
 * @param cert      Path to the PEM certificate chain (leaf first).
 * @param key       Path to the PEM private key.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_ctx_load_cert(
    tls_ctx_t*  ctx,
    const char* hostname,
    const char* cert,
    const char* key);

/**
 * @brief Load a PEM certificate chain and private key from memory.
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
extern int tls_ctx_load_cert_mem(
    tls_ctx_t*  ctx,
    const char* hostname,
    const void* cert_pem,
    size_t      cert_len,
    const void* key_pem,
    size_t      key_len);

/**
 * @brief Add a CA certificate file to the trust store.
 *
 * @param ctx      Context handle.
 * @param ca_file  Path to a PEM CA certificate (or bundle) file.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_ctx_load_ca(tls_ctx_t* ctx, const char* ca_file);

/**
 * @brief Load public-CA trust anchors: the system store plus a fallback.
 *
 * @param ctx               Context handle.
 * @param fallback_ca_file  PEM CA bundle path, or NULL for none.
 *
 * @return 0 if at least one source loaded, -1 if none did.
 */
extern int tls_ctx_load_system_ca(
    tls_ctx_t*  ctx,
    const char* fallback_ca_file);

/**
 * @brief Set whether a client verifies the server certificate.
 *
 * @param ctx     Context handle.
 * @param enable  true to verify (default), false to skip.
 */
extern void tls_ctx_verify_server(tls_ctx_t* ctx, bool enable);

/**
 * @brief Set whether a server requires a client certificate (mTLS).
 *
 * @param ctx     Context handle.
 * @param enable  true to require and verify, false to request none.
 */
extern void tls_ctx_verify_client(tls_ctx_t* ctx, bool enable);

/**
 * @brief Set the ALPN protocol list.
 *
 * @param ctx        Context handle.
 * @param protocols  Array of protocol strings (e.g. "h2", "http/1.1").
 * @param count      Number of protocols.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_ctx_set_alpn(
    tls_ctx_t*   ctx,
    const char** protocols,
    size_t       count);

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
 * @brief Close a connection and release it. Idempotent.
 *
 * @param tls  Connection handle.
 */
extern void        tls_close(tls_conn_t* tls);

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
 *  - NULL is returned only when the listener is closed; it no longer
 *    signals a handshake failure.
 *  - A handshake failure (bad cert, protocol mismatch, timeout) surfaces
 *    as -1 from the first tls_read/tls_write/tls_handshake, not here.
 *  - handshake_timeout_ms is measured from when the handshake begins
 *    (first I/O), not from accept. A handler that never reads is not
 *    bounded by it until then; rely on prompt handler reads plus
 *    transport/backlog limits.
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
 * @return Accepted connection, or NULL when the listener is closed.
 */
extern tls_conn_t*     tls_accept(tls_listener_t* ln);

/**
 * @brief Close and destroy a listener. Idempotent.
 *
 * @param ln  Listener handle.
 */
extern void            tls_close_listener(tls_listener_t* ln);

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
 * @brief Set the read deadline, in absolute monotonic milliseconds.
 *
 * @param tls          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void tls_set_read_deadline(tls_conn_t* tls, uint64_t deadline_ms);

/**
 * @brief Set the write deadline, in absolute monotonic milliseconds.
 *
 * @param tls          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
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
 *
 * On success ownership of @p fd is transferred to the returned
 * connection; on failure @p fd is closed.
 *
 * @param fd   Connected socket (ownership transferred on success).
 * @param ctx  TLS context.
 * @param opts TLS options (server_name, handshake_timeout_ms, etc.).
 *
 * @return TLS connection handle, or NULL on failure (fd is closed).
 */
extern tls_conn_t* tls_client_handshake_fd(
    platform_sock_t   fd,
    tls_ctx_t*        ctx,
    xylem_tls_opts_t* opts);
