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

#include "xylem/http/xylem-http-common.h"

#include <stddef.h>
#include <stdint.h>

typedef struct xylem_loop_s xylem_loop_t;

/* Opaque types */
typedef struct xylem_http_req_s     xylem_http_req_t;
typedef struct xylem_http_conn_s    xylem_http_conn_t;
typedef struct xylem_http_srv_s     xylem_http_srv_t;
typedef struct xylem_http_router_s  xylem_http_router_t;

/**
 * @brief Server request callback.
 *
 * Invoked when a complete HTTP request has been received. The
 * connection and request handles are valid only within this callback.
 *
 * @param conn      Connection handle for sending the response.
 * @param req       Parsed request (method, URL, headers, body).
 * @param userdata  User-supplied pointer from the server config.
 */
typedef void (*xylem_http_on_request_fn_t)(xylem_http_conn_t* conn,
                                           xylem_http_req_t* req,
                                           void* userdata);


/**
 * @brief Server configuration.
 *
 * Caller allocates on the stack and passes to xylem_http_srv_create().
 * The strings pointed to by host, tls_cert, and tls_key must remain
 * valid until xylem_http_srv_destroy() is called.
 */
typedef struct xylem_http_srv_cfg_s {
    const char*                  host;            /* bind address, e.g. "0.0.0.0" */
    uint16_t                     port;            /* bind port */
    xylem_http_on_request_fn_t   on_request;      /* request callback */
    void*                        userdata;        /* passed to on_request */
    const char*                  tls_cert;        /* PEM cert path, NULL for plain HTTP */
    const char*                  tls_key;         /* PEM key path, NULL for plain HTTP */
    size_t                       max_body_size;   /* max request body, 0 = default 1 MiB */
    uint64_t                     idle_timeout_ms; /* idle timeout, 0 = disabled, default 60000 */
} xylem_http_srv_cfg_t;

/**
 * @brief Create an HTTP server.
 *
 * The event loop is passed as a separate parameter, consistent
 * with xylem_tcp_listen and xylem_tls_listen.
 *
 * @param loop  Event loop the server runs on.
 * @param cfg   Server configuration.
 *
 * @return Server handle on success, NULL on failure.
 */
extern xylem_http_srv_t* xylem_http_srv_create(xylem_loop_t* loop,
                                                const xylem_http_srv_cfg_t* cfg);

/**
 * @brief Start the HTTP server.
 *
 * Binds to the configured host and port and begins accepting
 * connections. Uses xylem_tcp or xylem_tls depending on whether
 * tls_cert and tls_key are set in the config.
 *
 * @param srv  Server handle.
 *
 * @return 0 on success, -1 on failure (e.g. bind error).
 */
extern int xylem_http_srv_start(xylem_http_srv_t* srv);

/**
 * @brief Stop the HTTP server.
 *
 * Stops accepting new connections. Existing connections continue
 * processing until they complete or are closed.
 *
 * @param srv  Server handle.
 */
extern void xylem_http_srv_stop(xylem_http_srv_t* srv);

/**
 * @brief Destroy the HTTP server and free all resources.
 *
 * @param srv  Server handle.
 */
extern void xylem_http_srv_destroy(xylem_http_srv_t* srv);

/**
 * @brief Get the request method string.
 *
 * @param req  Request handle.
 *
 * @return Method string (e.g. "GET", "POST").
 */
extern const char* xylem_http_req_method(const xylem_http_req_t* req);

/**
 * @brief Get the request URL path string.
 *
 * @param req  Request handle.
 *
 * @return URL path string.
 */
extern const char* xylem_http_req_url(const xylem_http_req_t* req);

/**
 * @brief Get a request header value by name.
 *
 * Performs case-insensitive ASCII matching per RFC 7230.
 *
 * @param req   Request handle.
 * @param name  Header name to look up.
 *
 * @return Header value string, or NULL if not found.
 */
extern const char* xylem_http_req_header(const xylem_http_req_t* req,
                                          const char* name);

/**
 * @brief Get the request body data.
 *
 * @param req  Request handle.
 *
 * @return Pointer to body bytes, or NULL if no body.
 */
extern const void* xylem_http_req_body(const xylem_http_req_t* req);

/**
 * @brief Get the request body length.
 *
 * @param req  Request handle.
 *
 * @return Body length in bytes, or 0 if no body.
 */
extern size_t xylem_http_req_body_len(const xylem_http_req_t* req);

/**
 * @brief Send an HTTP response on a connection.
 *
 * Serializes a complete HTTP/1.1 response with status line,
 * Content-Type, Content-Length, and body. Custom headers are
 * written before auto-generated headers; a custom header whose
 * name matches an auto-generated one (case-insensitive) overrides it.
 *
 * @param conn          Connection handle.
 * @param status_code   HTTP status code (e.g. 200, 404).
 * @param content_type  Content-Type header value.
 * @param body          Response body, or NULL for empty body.
 * @param body_len      Body length in bytes.
 * @param headers       Custom response headers, or NULL for none.
 * @param header_count  Number of custom response headers.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_send(xylem_http_conn_t* conn,
                                 int status_code,
                                 const char* content_type,
                                 const void* body, size_t body_len,
                                 const xylem_http_hdr_t* headers,
                                 size_t header_count);

/**
 * @brief Begin a chunked transfer-encoded response.
 *
 * Sends the status line, Transfer-Encoding: chunked header,
 * Content-Type, and any custom headers. Does not send a body.
 * After this call, use xylem_http_conn_send_chunk() to send
 * body fragments and xylem_http_conn_end_chunked() to finish.
 *
 * @param conn          Connection handle.
 * @param status_code   HTTP status code (e.g. 200).
 * @param content_type  Content-Type header value, or NULL.
 * @param headers       Custom response headers, or NULL for none.
 * @param header_count  Number of custom response headers.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_start_chunked(xylem_http_conn_t* conn,
                                         int status_code,
                                         const char* content_type,
                                         const xylem_http_hdr_t* headers,
                                         size_t header_count);

/**
 * @brief Send a single chunk in a chunked response.
 *
 * Formats the data as a chunked transfer-encoding frame
 * ({hex_size}\r\n{data}\r\n) and sends it on the connection.
 *
 * @param conn  Connection handle.
 * @param data  Chunk data.
 * @param len   Chunk length in bytes. If 0, this is a no-op.
 *
 * @return 0 on success, -1 on failure (connection closed or not
 *         in chunked mode).
 */
extern int xylem_http_conn_send_chunk(xylem_http_conn_t* conn,
                                      const void* data, size_t len);

/**
 * @brief End a chunked transfer-encoded response.
 *
 * Sends the terminating zero-length chunk (0\r\n\r\n) and
 * handles keep-alive: resets the parser for the next request
 * or closes the connection.
 *
 * @param conn  Connection handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_end_chunked(xylem_http_conn_t* conn);

/**
 * @brief Send a partial content (206) or range-not-satisfiable (416) response.
 *
 * When range_start <= range_end and range_end < total_size, sends a
 * 206 Partial Content response with the Content-Range header set to
 * "bytes start-end/total". Otherwise sends a 416 Range Not Satisfiable
 * response with Content-Range set to "bytes * /total".
 *
 * @param conn          Connection handle.
 * @param content_type  Content-Type header value.
 * @param body          Response body slice, or NULL for 416.
 * @param body_len      Body length in bytes.
 * @param range_start   First byte position of the range.
 * @param range_end     Last byte position of the range (inclusive).
 * @param total_size    Total size of the complete resource.
 * @param headers       Custom response headers, or NULL for none.
 * @param header_count  Number of custom response headers.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_send_partial(xylem_http_conn_t* conn,
                                        const char* content_type,
                                        const void* body, size_t body_len,
                                        size_t range_start, size_t range_end,
                                        size_t total_size,
                                        const xylem_http_hdr_t* headers,
                                        size_t header_count);

/**
 * @brief Begin a Server-Sent Events stream.
 *
 * Sends status 200 with Content-Type: text/event-stream,
 * Cache-Control: no-cache, and Connection: keep-alive using
 * chunked transfer encoding internally.
 *
 * @param conn          Connection handle.
 * @param headers       Custom response headers, or NULL for none.
 * @param header_count  Number of custom response headers.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_start_sse(xylem_http_conn_t* conn,
                                     const xylem_http_hdr_t* headers,
                                     size_t header_count);

/**
 * @brief Send a single SSE event.
 *
 * Formats the event as "event: {event}\ndata: {data}\n\n".
 * When event is NULL, the "event:" line is omitted.
 * Multi-line data is split into separate "data:" lines.
 *
 * @param conn   Connection handle.
 * @param event  Event type string, or NULL for data-only.
 * @param data   Event data string.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_send_event(xylem_http_conn_t* conn,
                                      const char* event,
                                      const char* data);

/**
 * @brief Send a data-only SSE message.
 *
 * Equivalent to xylem_http_conn_send_event(conn, NULL, data).
 *
 * @param conn  Connection handle.
 * @param data  Event data string.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_send_sse_data(xylem_http_conn_t* conn,
                                         const char* data);

/**
 * @brief End a Server-Sent Events stream.
 *
 * Sends the terminating chunk and handles keep-alive or close.
 *
 * @param conn  Connection handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_conn_end_sse(xylem_http_conn_t* conn);

/**
 * @brief Close a client connection.
 *
 * @param conn  Connection handle.
 */
extern void xylem_http_conn_close(xylem_http_conn_t* conn);

/**
 * @brief Get a path parameter value extracted during routing.
 *
 * When xylem_http_router_dispatch matches a route containing
 * `:name` segments, the captured values are stored on the request
 * and can be retrieved by name.
 *
 * @param req   Request handle.
 * @param name  Parameter name (without the leading ':').
 *
 * @return Parameter value string, or NULL if not found.
 *
 * @note The returned pointer is valid until the request callback returns.
 */
extern const char* xylem_http_req_param(const xylem_http_req_t* req,
                                        const char* name);

/**
 * @brief Create a router for dispatching requests by method and path.
 *
 * @return Router handle on success, NULL on allocation failure.
 *
 * @note The caller must free with xylem_http_router_destroy().
 */
extern xylem_http_router_t* xylem_http_router_create(void);

/**
 * @brief Destroy a router and free all registered routes.
 *
 * @param router  Router handle, or NULL (no-op).
 */
extern void xylem_http_router_destroy(xylem_http_router_t* router);

/**
 * @brief Register a route.
 *
 * Pattern syntax:
 *   - Exact: "/api/users"
 *   - Path param: "/user/:id" (matches one segment, captures value)
 *   - Wildcard: "/static/*" (matches any suffix)
 *
 * Method is case-sensitive (e.g. "GET", "POST"). Pass NULL to
 * match all HTTP methods.
 *
 * @param router   Router handle.
 * @param method   HTTP method string, or NULL for all methods.
 * @param pattern  URL path pattern.
 * @param handler  Request handler callback.
 * @param userdata User-supplied pointer passed to handler.
 *
 * @return 0 on success, -1 on failure (duplicate route, bad args).
 */
extern int xylem_http_router_add(xylem_http_router_t* router,
                                 const char* method,
                                 const char* pattern,
                                 xylem_http_on_request_fn_t handler,
                                 void* userdata);

/**
 * @brief Dispatch a request to the best matching route.
 *
 * Matching priority: exact > path-param > wildcard prefix.
 * Among same type, longer patterns win. Specific method wins
 * over NULL (all-methods) wildcard. If no route matches, sends
 * a 404 Not Found response.
 *
 * @param router  Router handle.
 * @param conn    Connection handle.
 * @param req     Request handle.
 *
 * @return 0 if a route matched, -1 if 404 was sent.
 */
extern int xylem_http_router_dispatch(xylem_http_router_t* router,
                                      xylem_http_conn_t* conn,
                                      xylem_http_req_t* req);
