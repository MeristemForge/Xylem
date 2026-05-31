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

#include "platform/platform-socket.h"

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
extern xylem_tls_conn_t* tls_client_handshake(platform_sock_t fd,
                                              xylem_tls_ctx_t* ctx,
                                              xylem_tls_opts_t* opts);
