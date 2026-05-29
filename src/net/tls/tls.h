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

#include "xylem/net/xylem-tls.h"

#include "net/addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"

#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct _tls_sni_entry_s {
    char     hostname[256];
    SSL_CTX* ssl_ctx;
} _tls_sni_entry_t;

struct xylem_tls_ctx_s {
    SSL_CTX*          ssl_ctx;
    uint8_t*          alpn_wire;
    size_t            alpn_wire_len;
    FILE*             keylog_file;
    _tls_sni_entry_t* sni_entries;
    size_t            sni_count;
    size_t            sni_cap;
};

struct xylem_tls_conn_s {
    SSL*            ssl;
    iowait_t*      waiter;
    platform_sock_t fd;
    xylem_tls_ctx_t* ctx;
    addr_t          peer_addr;
    char            alpn[256];
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

struct xylem_tls_listener_s {
    iowait_t*        waiter;
    platform_sock_t  fd;
    xylem_tls_ctx_t* ctx;
    xylem_tls_opts_t opts;
    _Atomic int32_t  refcnt;
    _Atomic bool     closed;
};

/**
 * @brief Allocate a tls_conn and register its fd with the event loop.
 *
 * @param fd  Connected socket.
 *
 * @return Connection handle, or NULL on failure.
 */
extern xylem_tls_conn_t* tls_conn_create(platform_sock_t fd);

/**
 * @brief Increment connection reference count.
 *
 * @param tls  Connection handle.
 */
extern void tls_conn_ref(xylem_tls_conn_t* tls);

/**
 * @brief Decrement reference count; frees at zero.
 *
 * @param tls  Connection handle.
 */
extern void tls_conn_unref(xylem_tls_conn_t* tls);

/**
 * @brief Destroy connection unconditionally (no refcount check).
 *
 * @param tls  Connection handle.
 */
extern void tls_conn_destroy(xylem_tls_conn_t* tls);

/**
 * @brief Drive the SSL handshake to completion (async-aware).
 *
 * @param tls  Connection with ssl set to connect or accept state.
 *
 * @return 0 on success, -1 on failure.
 */
extern int tls_do_handshake(xylem_tls_conn_t* tls);

/**
 * @brief Set SNI and hostname verification on the SSL object.
 *
 * @param ssl          SSL object.
 * @param server_name  Hostname, or NULL.
 */
extern void tls_apply_server_name(SSL* ssl, const char* server_name);

/**
 * @brief Cache the negotiated ALPN protocol into tls->alpn.
 *
 * @param tls  Connection handle.
 */
extern void tls_cache_alpn(xylem_tls_conn_t* tls);

/**
 * @brief Handle SSL_ERROR_WANT_READ/WRITE by parking on the event loop.
 *
 * @param tls      Connection handle.
 * @param ssl_err  Result of SSL_get_error().
 * @param op_name  Operation name for logging.
 *
 * @return 0 to retry, 1 on peer close, -1 on fatal error.
 */
extern int tls_handle_io_block(xylem_tls_conn_t* tls, int ssl_err,
                               const char* op_name);

/**
 * @brief Perform TLS client handshake on an already-connected fd.
 *
 * @param fd   Connected socket (ownership transferred on success).
 * @param ctx  TLS context.
 * @param opts TLS options (server_name, timeout, etc.).
 *
 * @return TLS connection handle, or NULL on failure (fd is closed).
 */
extern xylem_tls_conn_t* tls_handshake(platform_sock_t fd,
                                        xylem_tls_ctx_t* ctx,
                                        xylem_tls_opts_t* opts);
