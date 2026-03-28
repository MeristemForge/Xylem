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
 * @brief Response writer handle.
 *
 * Alias for the connection handle, used in request callbacks to
 * build and send the HTTP response (set_status, set_header, write).
 * Conceptually equivalent to Go's http.ResponseWriter.
 */
typedef xylem_http_conn_t xylem_http_writer_t;

/**
 * @brief Server request callback.
 *
 * Invoked when a complete HTTP request has been received. The
 * writer and request handles are valid only within this callback.
 *
 * @param writer    Response writer for sending the response.
 * @param req       Parsed request (method, URL, headers, body).
 * @param userdata  User-supplied pointer from the server config.
 */
typedef void (*xylem_http_on_request_fn_t)(xylem_http_writer_t* writer,
                                           xylem_http_req_t* req,
                                           void* userdata);

/**
 * @brief Upgrade request callback.
 *
 * Invoked when a client sends a request with the Upgrade header
 * and Connection: Upgrade. The callback receives the same parameters
 * as on_request. To accept the upgrade, call
 * xylem_http_writer_accept_upgrade() within this callback. If the
 * callback returns without accepting, the connection is closed.
 *
 * @param writer    Response writer for sending the 101 response.
 * @param req       Parsed request (contains Upgrade header value).
 * @param userdata  User-supplied pointer from the server config.
 */
typedef void (*xylem_http_on_upgrade_fn_t)(xylem_http_writer_t* writer,
                                           xylem_http_req_t* req,
                                           void* userdata);

/**
 * @brief Middleware callback.
 *
 * Invoked before the route handler during xylem_http_router_dispatch().
 * Middleware functions run in registration order. Return 0 to continue
 * to the next middleware or the route handler. Return -1 to abort the
 * chain (the middleware must send a response before returning -1).
 *
 * @param writer    Response writer for sending a response.
 * @param req       Parsed request (method, URL, headers, body).
 * @param userdata  User-supplied pointer from xylem_http_router_use().
 *
 * @return 0 to continue, -1 to abort the chain.
 */
typedef int (*xylem_http_middleware_fn_t)(xylem_http_writer_t* writer,
                                         xylem_http_req_t* req,
                                         void* userdata);


/**
 * @brief Gzip response compression options.
 *
 * Configure via xylem_http_srv_set_gzip(). When enabled, responses
 * whose Content-Type matches a compressible MIME type and whose body
 * exceeds min_size are automatically gzip-compressed if the client
 * advertises Accept-Encoding: gzip.
 */
typedef struct {
    bool        enabled;     /**< Global on/off switch, default false. */
    int         level;       /**< Compression level 1-9, 0 = default (6). */
    size_t      min_size;    /**< Minimum body size to compress, default 1024. */
    const char* mime_types;  /**< Comma-separated compressible MIME types,
                                  NULL = built-in defaults. */
} xylem_http_gzip_opts_t;

/**
 * @brief Server configuration.
 *
 * Caller allocates on the stack and passes to xylem_http_listen().
 * The strings pointed to by host, tls_cert, and tls_key must remain
 * valid until xylem_http_close_server() is called.
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
    xylem_http_on_upgrade_fn_t   on_upgrade;      /* upgrade callback, NULL = reject with 501 */
} xylem_http_srv_cfg_t;

/**
 * @brief Create and start an HTTP server.
 *
 * Allocates the server, configures it from cfg, binds to the
 * configured host and port, and begins accepting connections.
 * Uses xylem_tcp or xylem_tls depending on whether tls_cert
 * and tls_key are set in the config.
 *
 * Consistent with xylem_tcp_listen and xylem_ws_listen.
 *
 * @param loop  Event loop the server runs on.
 * @param cfg   Server configuration.
 *
 * @return Server handle on success, NULL on failure.
 */
extern xylem_http_srv_t* xylem_http_listen(xylem_loop_t* loop,
                                           const xylem_http_srv_cfg_t* cfg);

/**
 * @brief Stop the HTTP server and free all resources.
 *
 * Stops accepting new connections, closes the listener, and
 * frees the server handle. Existing connections continue
 * processing until they complete or are closed.
 *
 * @param srv  Server handle, or NULL (no-op).
 */
extern void xylem_http_close_server(xylem_http_srv_t* srv);

/**
 * @brief Configure gzip response compression.
 *
 * When enabled, the server automatically compresses response bodies
 * that match the configured MIME types and exceed the minimum size
 * threshold, provided the client sends Accept-Encoding: gzip.
 *
 * @param srv   Server handle.
 * @param opts  Gzip options. The struct is copied; the caller may
 *              free it after this call. mime_types string must remain
 *              valid for the server lifetime if non-NULL.
 */
extern void xylem_http_srv_set_gzip(xylem_http_srv_t* srv,
                                    const xylem_http_gzip_opts_t* opts);

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
 * @brief Build an SSE-formatted message string.
 *
 * Allocates and returns a string in SSE wire format:
 * "event: {event}\ndata: {data}\n\n". When event is NULL,
 * the "event:" line is omitted. Multi-line data is split
 * into separate "data:" lines per the SSE specification.
 *
 * The caller must free() the returned pointer.
 *
 * @param event  Event type string, or NULL for data-only.
 * @param data   Event data string (must not be NULL).
 * @param len    If non-NULL, receives the byte length of the result.
 *
 * @return Heap-allocated SSE string, or NULL on failure.
 */
extern char* xylem_http_sse_build(const char* event,
                                  const char* data,
                                  size_t* len);

/**
 * @brief Buffer a response header.
 *
 * Accumulates headers until the first xylem_http_writer_write() call,
 * which flushes them automatically. If a header with the same name
 * already exists in the buffer, its value is replaced (last-write-wins).
 *
 * Must be called before the first write. Returns -1 if headers have
 * already been sent.
 *
 * @param writer  Response writer handle.
 * @param name    Header name (copied internally).
 * @param value   Header value (copied internally).
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_writer_set_header(xylem_http_writer_t* writer,
                                        const char* name,
                                        const char* value);

/**
 * @brief Set the response status code.
 *
 * Must be called before the first write. If not called, defaults
 * to 200. Returns -1 if headers have already been sent.
 *
 * @param writer       Response writer handle.
 * @param status_code  HTTP status code (e.g. 200, 404).
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_writer_set_status(xylem_http_writer_t* writer,
                                        int status_code);

/**
 * @brief Write response body data.
 *
 * On the first call, automatically sends the status line and all
 * buffered headers with Transfer-Encoding: chunked. Subsequent
 * calls send additional chunks. The framework automatically
 * finalizes the response when the request callback returns.
 *
 * @param writer  Response writer handle.
 * @param data    Body data to write.
 * @param len     Data length in bytes. If 0, this is a no-op.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_writer_write(xylem_http_writer_t* writer,
                                   const void* data, size_t len);

/**
 * @brief Close the underlying connection.
 *
 * @param writer  Response writer handle.
 */
extern void xylem_http_writer_close(xylem_http_writer_t* writer);

/**
 * @brief Accept an HTTP Upgrade request.
 *
 * Sends a 101 Switching Protocols response with the Upgrade and
 * Connection: Upgrade headers. Detaches the underlying transport
 * handle from HTTP connection management: stops the idle timer,
 * stops HTTP parsing, and transfers ownership to the caller.
 *
 * Must be called from within the on_upgrade callback. Calling
 * from any other context returns -1.
 *
 * After a successful call, the caller owns the transport handle
 * and is responsible for reading, writing, and closing it.
 *
 * @param writer     Response writer handle (from on_upgrade callback).
 * @param transport  Output: underlying transport handle. For plain
 *                   HTTP this is xylem_tcp_conn_t*; for HTTPS this
 *                   is xylem_tls_t*. Cast as needed.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_writer_accept_upgrade(xylem_http_writer_t* writer,
                                            void** transport);

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
 *   - Wildcard: "/static/" + "*" (matches any suffix)
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
 * @brief Register a global middleware on the router.
 *
 * Middleware functions run in registration order before the matched
 * route handler during xylem_http_router_dispatch(). If any middleware
 * returns -1, the chain is aborted and the route handler is not called.
 * The middleware that aborts is responsible for sending a response.
 *
 * @param router   Router handle.
 * @param fn       Middleware callback.
 * @param userdata User-supplied pointer passed to fn.
 *
 * @return 0 on success, -1 on failure (bad args, allocation error).
 */
extern int xylem_http_router_use(xylem_http_router_t* router,
                                 xylem_http_middleware_fn_t fn,
                                 void* userdata);

/**
 * @brief Dispatch a request to the best matching route.
 *
 * Runs all registered middleware in order before calling the
 * matched route handler. If any middleware returns -1, the chain
 * is aborted and the handler is not called.
 *
 * Matching priority: exact > path-param > wildcard prefix.
 * Among same type, longer patterns win. Specific method wins
 * over NULL (all-methods) wildcard. If no route matches, sends
 * a 404 Not Found response.
 *
 * @param router  Router handle.
 * @param writer  Response writer handle.
 * @param req     Request handle.
 *
 * @return 0 if a route matched, -1 if 404 was sent or middleware aborted.
 */
extern int xylem_http_router_dispatch(xylem_http_router_t* router,
                                      xylem_http_writer_t* writer,
                                      xylem_http_req_t* req);

/**
 * @brief Static file server configuration.
 *
 * Pass to xylem_http_static_serve() to register a static file
 * handler on a router prefix.
 */
typedef struct {
    const char* root;          /**< File system root directory. */
    const char* index_file;    /**< Default document, NULL = "index.html". */
    int         max_age;       /**< Cache-Control max-age seconds, 0 = omit. */
    bool        precompressed; /**< Look for .gz pre-compressed files. */
} xylem_http_static_opts_t;

/**
 * @brief Register a static file handler on a router.
 *
 * Maps URL paths under prefix to files under opts->root.
 * Supports GET and HEAD methods. Returns 404 for missing files,
 * 405 for other methods, 403 for directory listings without an
 * index file. Prevents path traversal attacks.
 *
 * When precompressed is true and the client accepts gzip, the
 * handler looks for a .gz sibling file before reading the original.
 *
 * @param router  Router handle.
 * @param prefix  URL prefix (e.g. "/static"). Must end with "/" + "*"
 *                or the function appends it internally.
 * @param opts    Static file options. The struct is copied; root and
 *                index_file strings must remain valid for the router
 *                lifetime.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_static_serve(xylem_http_router_t* router,
                                   const char* prefix,
                                   const xylem_http_static_opts_t* opts);
