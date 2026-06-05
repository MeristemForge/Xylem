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

/*
 * Internal TLS engine.
 *
 * Defines the real connection/context/listener structs and the engine
 * API (tls_*), built on the backend TLS interface and the coroutine
 * runtime. The
 * public xylem_tls_* surface (xylem-tls.c) is a thin opaque-handle shim
 * over these; other internal consumers that need the engine directly --
 * the HTTPS transport factory (http-transport-tls.c), which runs the
 * client handshake over a proxy-tunnel fd via tls_client_handshake_fd --
 * include this header instead of the public one.
 *
 * Not part of the public API. Do not include outside the TLS/HTTP/WS
 * modules.
 */

_Pragma("once")

#include "xylem/net/xylem-tls.h"
#include "xylem/sync/xylem-mutex.h"

#include "net/addr.h"
#include "net/tls/tls-backend.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Engine type names. The public header declares the opaque xylem_tls_*
 * handle typedefs; the engine works on these concrete tls_* structs. The
 * public shim wraps them (first-member equivalence) so the two layers
 * never share a struct tag.
 *
 * The options struct has no internal alias: xylem_tls_opts_t is a
 * transparent value type whose fields the caller fills directly, so the
 * engine uses the public type as-is (the http engine treats its
 * xylem_http_*_opts_t the same way).
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
    tls_backend_conn_t* be;     /* replaces ssl, rbio, wbio */
    char*            rbuf;
    char*            wbuf;
    xylem_mutex_t*   ssl_mu;
    xylem_mutex_t*   rd_mu;
    xylem_mutex_t*   wr_mu;
    iowait_t*        waiter;
    platform_sock_t  fd;
    tls_ctx_t*       ctx;
    addr_t           peer_addr;
    char             alpn[32];
    _Atomic int32_t  refcnt;
    _Atomic bool     closed;
};

struct tls_listener_s {
    iowait_t*        waiter;
    platform_sock_t  fd;
    tls_ctx_t*       ctx;
    xylem_tls_opts_t opts;
    _Atomic int32_t  refcnt;
    _Atomic bool     closed;
};

/* ===================================================================== *
 *  Context lifecycle and configuration
 * ===================================================================== */

extern tls_ctx_t* tls_ctx_create(void);
extern void       tls_ctx_destroy(tls_ctx_t* ctx);
extern int        tls_ctx_set_keylog(tls_ctx_t* ctx, const char* path);

extern int tls_ctx_load_cert(tls_ctx_t* ctx, const char* hostname,
                             const char* cert, const char* key);
extern int tls_ctx_load_cert_mem(tls_ctx_t* ctx, const char* hostname,
                                 const void* cert_pem, size_t cert_len,
                                 const void* key_pem, size_t key_len);
extern int tls_ctx_load_ca(tls_ctx_t* ctx, const char* ca_file);
extern int tls_ctx_load_system_ca(tls_ctx_t* ctx);

extern void tls_ctx_verify_server(tls_ctx_t* ctx, bool enable);
extern void tls_ctx_verify_client(tls_ctx_t* ctx, bool enable);
extern int  tls_ctx_set_alpn(tls_ctx_t* ctx, const char** protocols,
                             size_t count);

/* ===================================================================== *
 *  Connections and listeners
 * ===================================================================== */

extern tls_conn_t* tls_dial(const char* host, uint16_t port,
                            tls_ctx_t* ctx, xylem_tls_opts_t* opts);
extern void        tls_close(tls_conn_t* tls);

extern tls_listener_t* tls_listen(const char* host, uint16_t port,
                                  tls_ctx_t* ctx, xylem_tls_opts_t* opts);
extern tls_conn_t*     tls_accept(tls_listener_t* ln);
extern void            tls_close_listener(tls_listener_t* ln);

extern int tls_read(tls_conn_t* tls, void* buf, int len);
extern int tls_write(tls_conn_t* tls, const void* data, int len);

extern void tls_set_read_deadline(tls_conn_t* tls, uint64_t deadline_ms);
extern void tls_set_write_deadline(tls_conn_t* tls, uint64_t deadline_ms);

extern int tls_remote_addr(tls_conn_t* tls, char* host, size_t host_len,
                           uint16_t* port);
extern int tls_local_addr(tls_conn_t* tls, char* host, size_t host_len,
                          uint16_t* port);
extern int tls_listener_addr(tls_listener_t* ln, char* host, size_t host_len,
                             uint16_t* port);

extern const char* tls_get_alpn(tls_conn_t* tls);

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
extern tls_conn_t* tls_client_handshake_fd(platform_sock_t fd,
                                           tls_ctx_t* ctx,
                                           xylem_tls_opts_t* opts);
