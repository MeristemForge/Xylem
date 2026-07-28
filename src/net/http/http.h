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

/**
 * URL / header value types shared across the HTTP module. The stateless
 * helpers that operate on them are declared in http-utils.h.
 */

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

typedef xylem_http_req_t http_req_t;
typedef xylem_http_res_t http_res_t;
typedef xylem_http_srv_t http_srv_t;
typedef xylem_http_writer_t http_writer_t;

typedef enum http_writer_state_e {
    HTTP_WRITER_OPEN,
    HTTP_WRITER_FINISHED,
    HTTP_WRITER_HIJACKED,
    HTTP_WRITER_ABORTED,
} http_writer_state_t;

typedef struct http_writer_ops_s {
    int (*write)(http_writer_t* writer, const void* data, size_t len);
    int (*flush)(http_writer_t* writer);
    int (*finish)(http_writer_t* writer);
    int (*upgrade)(http_writer_t* writer, void** transport);
    int (*hijack)(http_writer_t* writer, void** transport);
} http_writer_ops_t;

typedef struct http1_response_s {
    http_transport_t* transport;
    uint8_t*          body_buf;
    size_t            body_buf_len;
} http1_response_t;

extern const http_writer_ops_t http1_writer_ops;

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
};

struct xylem_http_res_s {
    int            status_code;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
    size_t         body_cap;
};

struct xylem_http_writer_s {
    const http_writer_ops_t* ops;
    void*                    impl;
    int                      status_code;
    http_header_t*           headers;
    size_t                   header_count;
    size_t                   header_cap;
    http_writer_state_t      state;
    bool                     started;
    bool                     headers_sent;
};

struct xylem_http_srv_s {
    void*                   listener;
    void                    (*close_listener)(void* listener);
    void                    (*destroy_listener)(void* listener);
    void*                   transport_ctx;        /* transport-owned, e.g. TLS ctx. */
    void                    (*transport_ctx_free)(void* ctx); /* NULL if none. */
    xylem_http_handler_fn_t handler;
    void*                   userdata;
    char                    host[INET6_ADDRSTRLEN];
    uint16_t                port;
    xylem_http_handler_fn_t on_upgrade;
    void*                   upgrade_userdata;
    uint64_t                idle_timeout_ms;
    uint64_t                read_header_timeout_ms;
    uint64_t                write_timeout_ms;
    _Atomic int32_t         active_conns; /* refcount: owner + accept + conns */
    _Atomic bool            closing;
};

typedef struct http_srv_conn_ctx_s {
    http_srv_t*      srv;
    http_transport_t transport;
    char             remote_host[INET6_ADDRSTRLEN];
    uint16_t         remote_port;
} http_srv_conn_ctx_t;

/* Protocol engine (http.c) */

/**
 * @brief Finalize a server response.
 *
 * @param writer  Response writer.
 *
 * @return 0 on success, -1 on error.
 */
extern int http_writer_finish(http_writer_t* writer);

/**
 * @brief Report whether a handler has started constructing a response.
 *
 * @param writer  Response writer.
 */
extern bool http_writer_started(const http_writer_t* writer);

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
 * @brief Drop one reference to a server, freeing it on the last release.
 *
 * `active_conns` is a reference count, not just a gauge: the owner handle
 * holds one ref (released by xylem_http_close / xylem_http_shutdown), the
 * accept coroutine holds one, and every in-flight connection coroutine
 * holds one. Whichever party releases the final ref frees the server, so
 * a closing owner never frees `srv` out from under a coroutine still
 * reading its fields (data race / use-after-free).
 *
 * @param s  Server whose reference is being released. Must not be touched
 *           by the caller after this returns.
 */
extern void http_srv_unref(http_srv_t* s);

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
