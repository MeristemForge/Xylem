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
 * Core header for the HTTP module.
 *
 * Collects the types the HTTP translation units share: the transport
 * vtable, the URL/header value types, and the request/response/server
 * structures, plus the protocol-engine entry points. Stateless helpers
 * are declared in http-utils.h; the transport factories in
 * http-transport-tcp.h / http-transport-tls.h.
 *
 * Not part of the public API. Do not include outside the HTTP/WS modules.
 */

_Pragma("once")

#include "xylem/net/http/xylem-http.h"
#include "xylem/xylem-utils.h"

#include "platform/platform-socket.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Transport abstraction
 *
 * The protocol engine talks to the network exclusively through this
 * vtable. Whether the bytes travel over plain TCP or TLS is decided by
 * which factory builds the transport, never by a conditional inside the
 * engine. A build without TLS links the stub factories, which return a
 * NULL transport; no macros leak into the engine or the dispatch layer.
 */

typedef struct http_transport_s {
    void* conn;
    int  (*read)(void* conn, void* buf, int len);
    int  (*write)(void* conn, const void* data, int len);
    void (*close)(void* conn);
    void (*set_rd_deadline)(void* conn, uint64_t ms);
    void (*set_wr_deadline)(void* conn, uint64_t ms);
    int  (*remote_addr)(void* conn, char* host, size_t host_len, uint16_t* port);
    int  (*local_addr)(void* conn, char* host, size_t host_len, uint16_t* port);
    int  (*shutdown_wr)(void* conn); /* May be NULL (e.g. TLS). */
} http_transport_t;

typedef http_transport_t (*http_dial_fn_t)(const char* host, uint16_t port,
                                           uint64_t timeout_ms, void* ctx);

/* URL / header value types shared across the HTTP module. The stateless
 * helpers that operate on them are declared in http-utils.h. */

typedef struct http_url_s {
    char     scheme[8];
    char     host[256];
    uint16_t port;
    char     path[2048];
} http_url_t;

typedef struct http_header_s {
    char* name;
    char* value;
} http_header_t;

/* Request / response / server structures */

typedef struct http_router_param_s {
    char* key;
    char* value;
} http_router_param_t;

typedef struct http_req_s {
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
} http_req_t;

typedef struct http_res_s {
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
} http_res_t;

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

typedef struct http_srv_conn_ctx_s {
    http_srv_t*      srv;
    http_transport_t transport;
    char             remote_host[INET6_ADDRSTRLEN];
    uint16_t         remote_port;
} http_srv_conn_ctx_t;

/* Protocol engine (http.c) */

/**
 * @brief Write response body data (Content-Length or chunked).
 *
 * @param res   Internal response.
 * @param data  Body bytes.
 * @param len   Number of bytes.
 *
 * @return 0 on success, -1 on error.
 */
extern int http_res_write(http_res_t* res, const void* data, size_t len);

/**
 * @brief Finalize a response, emitting any buffered body and terminator.
 *
 * @param res  Internal response.
 */
extern void http_res_finalize(http_res_t* res);

/**
 * @brief Detach the underlying transport without writing any bytes.
 *
 * The engine relinquishes ownership of the connection and emits nothing;
 * the caller controls every subsequent byte (status line, framing, or a
 * raw tunnel). This is the shared primitive behind protocol upgrades and
 * CONNECT-style proxying.
 *
 * @param res        Internal response.
 * @param transport  Out: the detached transport handle (caller owns it).
 *
 * @return 0 on success, -1 on error.
 */
extern int http_res_hijack(http_res_t* res, void** transport);

/**
 * @brief Emit a 101 Switching Protocols response and detach the transport.
 *
 * @param res        Internal response.
 * @param transport  Out: the detached transport handle (caller owns it).
 *
 * @return 0 on success, -1 on error.
 */
extern int http_res_upgrade(http_res_t* res, void** transport);

/**
 * @brief Apply server options onto a freshly allocated server, filling
 *        in defaults for any unset timeout.
 *
 * @param srv   Server to initialize (handler/userdata already set).
 * @param opts  Server options, or NULL for all defaults.
 */
extern void http_srv_init(http_srv_t* srv, const xylem_http_srv_opts_t* opts);

/**
 * @brief Server connection coroutine entry.
 *
 * @param arg  Heap-allocated http_srv_conn_ctx_t (freed inside).
 */
extern void http_srv_conn_coroutine(void* arg);

/**
 * @brief Perform an HTTP client request over a caller-provided transport.
 *
 * @param method     HTTP method string.
 * @param url        Full URL.
 * @param body       Request body, or NULL.
 * @param body_len   Body length in bytes.
 * @param content_type  Content-Type value, or NULL.
 * @param headers    Custom headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts       Client options, or NULL.
 * @param absolute_form  Write the request target in absolute-form (proxy
 *                       forwarding); origin-form when false.
 * @param dial_fn    Dial callback that creates a transport for a host.
 * @param dial_ctx   Opaque context passed to dial_fn.
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
    bool                         absolute_form,
    http_dial_fn_t               dial_fn,
    void*                        dial_ctx);
