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

#include "xylem/net/xylem-http.h"
#include "xylem/xylem-utils.h"

#include "http-utils.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void* conn;
    int  (*read)(void* conn, void* buf, int len);
    int  (*write)(void* conn, const void* data, int len);
    void (*close)(void* conn);
    void (*set_rd_deadline)(void* conn, uint64_t ms);
    void (*set_wr_deadline)(void* conn, uint64_t ms);
} http_transport_t;

typedef http_transport_t (*http_dial_fn_t)(const char* host, uint16_t port,
                                           uint64_t timeout_ms, void* ctx);

typedef struct http_srv_s {
    void*                   listener;
    void                    (*close_listener)(void* listener);
    xylem_http_handler_fn_t handler;
    void*                   userdata;
    uint16_t                port;
    xylem_http_gzip_opts_t  gzip_opts;
    xylem_http_handler_fn_t on_upgrade;
    void*                   upgrade_userdata;
    size_t                  max_body_size;
    uint64_t                idle_timeout_ms;
    uint64_t                header_timeout_ms;
    uint64_t                max_requests;
    _Atomic int32_t         active_conns;
    bool                    closing;
} http_srv_t;

typedef struct {
    http_srv_t*      srv;
    http_transport_t transport;
} http_srv_conn_ctx_t;

/**
 * @brief Server connection coroutine entry.
 *
 * @param arg  Heap-allocated http_srv_conn_ctx_t (freed inside).
 */
extern void http_srv_conn_coroutine(void* arg);

/**
 * Perform an HTTP client request.
 *
 * @param method        HTTP method string (e.g. "GET").
 * @param url           Full URL string.
 * @param body          Request body, or NULL.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type value, or NULL.
 * @param opts          Client options (timeout, headers, cookie_jar, etc.).
 * @param dial_fn       Dial callback that creates a transport for a given host.
 * @param dial_ctx      Opaque context passed to dial_fn.
 *
 * @return Response object on success, NULL on failure. Caller frees via
 *         xylem_http_res_destroy().
 */
extern xylem_http_res_t* http_do_request(
    const char*              method,
    const char*              url,
    const void*              body,
    size_t                   body_len,
    const char*              content_type,
    const xylem_http_opts_t* opts,
    http_dial_fn_t           dial_fn,
    void*                    dial_ctx);
