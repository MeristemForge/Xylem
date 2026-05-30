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

#include "xylem/net/http/xylem-http.h"
#include "xylem/xylem-utils.h"

#include "http-utils.h"
#include "platform/platform-socket.h"

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

extern xylem_http_srv_t* http_listen_tcp(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts);

extern xylem_http_res_t* http_request_tcp(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

extern xylem_http_srv_t* http_listen_tls(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts);

extern xylem_http_res_t* http_request_tls(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

typedef struct {
    char* key;
    char* value;
} http_router_param_t;

struct xylem_http_req_s {
    char              method[16];
    char*             url;
    size_t            url_len;
    http_header_t*    headers;
    size_t            header_count;
    size_t            header_cap;
    uint8_t*          body;
    size_t            body_len;
    size_t            body_cap;
    char              remote_host[INET6_ADDRSTRLEN];
    uint16_t          remote_port;
    http_router_param_t* router_params;
    size_t               router_param_count;
    void*                _mw_chain;
};

struct xylem_http_res_s {
    int            status_code;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
    size_t         body_cap;
    http_transport_t* _transport;
    bool              _headers_sent;
    uint8_t*       _body_buf;
    size_t         _body_buf_len;
};

typedef struct http_srv_s {
    void*                   listener;
    void                    (*close_listener)(void* listener);
    xylem_http_handler_fn_t handler;
    void*                   userdata;
    char                    host[INET6_ADDRSTRLEN];
    uint16_t                port;
    xylem_http_handler_fn_t on_upgrade;
    void*                   upgrade_userdata;
    uint64_t                idle_timeout_ms;
    uint64_t                read_header_timeout_ms;
    uint64_t                write_timeout_ms;
    _Atomic int32_t         active_conns;
    bool                    closing;
} http_srv_t;

typedef struct {
    http_srv_t*      srv;
    http_transport_t transport;
    char             remote_host[INET6_ADDRSTRLEN];
    uint16_t         remote_port;
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
 * @param req       Populated request object.
 * @param opts      Client options (timeout, proxy, etc.), or NULL.
 * @param dial_fn   Dial callback that creates a transport for a given host.
 * @param dial_ctx  Opaque context passed to dial_fn.
 *
 * @return Response object on success, NULL on failure. Caller frees via
 *         xylem_http_res_destroy().
 */
extern xylem_http_res_t* http_do_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts,
    bool                         use_proxy,
    http_dial_fn_t               dial_fn,
    void*                        dial_ctx);

