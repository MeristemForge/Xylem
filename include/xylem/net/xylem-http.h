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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* name;  /**< Header name. */
    const char* value; /**< Header value. */
} xylem_http_hdr_t;

typedef struct xylem_http_req_s        xylem_http_req_t;
typedef struct xylem_http_res_s        xylem_http_res_t;
typedef struct xylem_http_srv_s        xylem_http_srv_t;
typedef struct xylem_http_router_s     xylem_http_router_t;
typedef struct xylem_http_cookie_jar_s xylem_http_cookie_jar_t;
typedef struct xylem_http_multipart_s  xylem_http_multipart_t;

/**
 * @brief Request handler invoked per HTTP request.
 *
 * Runs in its own coroutine. Write the response via res before
 * returning; the framework auto-finalizes on return.
 *
 * @param res       Response writer.
 * @param req       Parsed request.
 * @param userdata  Opaque pointer from listen or router_add.
 */
typedef void (*xylem_http_handler_fn_t)(
    xylem_http_res_t* res,
    xylem_http_req_t* req,
    void*             userdata);

/**
 * @brief Middleware invoked before the route handler.
 *
 * @param res       Response writer.
 * @param req       Parsed request.
 * @param userdata  Opaque pointer from router_use.
 *
 * @return 0 to continue to next middleware/handler, -1 to abort
 *         (must send a response before returning -1).
 */
typedef int (*xylem_http_middleware_fn_t)(
    xylem_http_res_t* res,
    xylem_http_req_t* req,
    void*             userdata);

typedef struct {
    size_t                  max_body_size;     /**< 0 = default 1 MiB. */
    uint64_t                idle_timeout_ms;   /**< 0 = default 60000 ms. */
    uint64_t                header_timeout_ms; /**< 0 = default 10000 ms. */
    uint64_t                max_requests;      /**< 0 = unlimited. */
    xylem_http_handler_fn_t on_upgrade;        /**< Upgrade handler, NULL = reject 501. */
    void*                   upgrade_userdata;  /**< Passed to on_upgrade. */
} xylem_http_srv_opts_t;

/**
 * @brief Start an HTTP server.
 *
 * Binds to host:port and begins accepting connections. Each
 * connection runs in its own coroutine. Must be called from
 * within a running xylem runtime.
 *
 * @param host      Bind address, or NULL for any.
 * @param port      Bind port. 0 for OS-assigned.
 * @param handler   Request handler.
 * @param userdata  Passed to handler.
 * @param opts      Server options, or NULL for defaults.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_http_srv_t* xylem_http_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts);

/**
 * @brief Stop the server and free resources.
 *
 * @param srv  Server handle, or NULL (no-op).
 */
extern void xylem_http_close(xylem_http_srv_t* srv);

/**
 * @brief Gracefully shut down the server.
 *
 * Stops accepting new connections and waits up to timeout_ms for
 * active connections to finish. Force-closes on timeout.
 *
 * @param srv         Server handle.
 * @param timeout_ms  Maximum wait time, 0 = immediate close.
 *
 * @return 0 on clean shutdown, -1 on timeout.
 */
extern int xylem_http_shutdown(xylem_http_srv_t* srv, uint64_t timeout_ms);

/**
 * @brief Get the listening port.
 *
 * Useful when started with port 0 (OS-assigned).
 *
 * @param srv  Server handle.
 *
 * @return Port number, or 0 on error.
 */
extern uint16_t xylem_http_srv_port(xylem_http_srv_t* srv);

typedef struct {
    bool        enabled;    /**< Global on/off switch. */
    int         level;      /**< Compression level 1-9, 0 = default (6). */
    size_t      min_size;   /**< Minimum body size to compress, default 1024. */
    const char* mime_types; /**< Comma-separated types, NULL = built-in defaults. */
} xylem_http_gzip_opts_t;

/**
 * @brief Configure gzip response compression.
 *
 * @param srv   Server handle.
 * @param opts  Gzip options.
 */
extern void xylem_http_srv_set_gzip(xylem_http_srv_t* srv,
                                    const xylem_http_gzip_opts_t* opts);

/**
 * @brief Get the request method.
 *
 * @param req  Request handle.
 *
 * @return Method string (e.g. "GET"), or NULL.
 */
extern const char* xylem_http_req_method(const xylem_http_req_t* req);

/**
 * @brief Get the request URL path.
 *
 * @param req  Request handle.
 *
 * @return URL path string, or NULL.
 */
extern const char* xylem_http_req_url(const xylem_http_req_t* req);

/**
 * @brief Get a request header value by name (case-insensitive).
 *
 * @param req   Request handle.
 * @param name  Header name.
 *
 * @return Header value, or NULL if not found.
 */
extern const char* xylem_http_req_header(const xylem_http_req_t* req,
                                         const char* name);

/**
 * @brief Get the request body.
 *
 * @param req  Request handle.
 *
 * @return Body bytes, or NULL if no body.
 */
extern const void* xylem_http_req_body(const xylem_http_req_t* req);

/**
 * @brief Get the request body length.
 *
 * @param req  Request handle.
 *
 * @return Body length in bytes.
 */
extern size_t xylem_http_req_body_len(const xylem_http_req_t* req);

/**
 * @brief Get a captured path parameter by name.
 *
 * @param req   Request handle.
 * @param name  Parameter name (without leading ':').
 *
 * @return Parameter value, or NULL if not found.
 */
extern const char* xylem_http_req_param(const xylem_http_req_t* req,
                                        const char* name);

/**
 * @brief Set the response status code.
 *
 * Must be called before the first write. Defaults to 200.
 *
 * @param res   Response handle.
 * @param code  HTTP status code.
 *
 * @return 0 on success, -1 if headers already sent.
 */
extern int xylem_http_res_set_status(xylem_http_res_t* res, int code);

/**
 * @brief Buffer a response header.
 *
 * Must be called before the first write.
 *
 * @param res    Response handle.
 * @param name   Header name.
 * @param value  Header value.
 *
 * @return 0 on success, -1 if headers already sent.
 */
extern int xylem_http_res_set_header(xylem_http_res_t* res,
                                     const char* name, const char* value);

/**
 * @brief Write response body data.
 *
 * First call flushes status line and headers.
 *
 * @param res   Response handle.
 * @param data  Body data.
 * @param len   Data length in bytes.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_http_res_write(xylem_http_res_t* res,
                                const void* data, size_t len);

/**
 * @brief Close the underlying connection.
 *
 * @param res  Response handle.
 */
extern void xylem_http_res_close(xylem_http_res_t* res);

/**
 * @brief Accept an HTTP Upgrade request.
 *
 * Sends 101 Switching Protocols and detaches the transport.
 * After this call the HTTP layer no longer manages the connection.
 *
 * @param res        Response handle.
 * @param transport  Output: underlying connection handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_res_upgrade(xylem_http_res_t* res, void** transport);

/**
 * @brief Get the response status code.
 *
 * @param res  Response handle.
 *
 * @return HTTP status code, or 0 if res is NULL.
 */
extern int xylem_http_res_status(const xylem_http_res_t* res);

/**
 * @brief Get a response header value by name (case-insensitive).
 *
 * @param res   Response handle.
 * @param name  Header name.
 *
 * @return Header value, or NULL if not found.
 */
extern const char* xylem_http_res_header(const xylem_http_res_t* res,
                                         const char* name);

/**
 * @brief Get the response body.
 *
 * @param res  Response handle.
 *
 * @return Body bytes, or NULL if no body.
 */
extern const void* xylem_http_res_body(const xylem_http_res_t* res);

/**
 * @brief Get the response body length.
 *
 * @param res  Response handle.
 *
 * @return Body length in bytes.
 */
extern size_t xylem_http_res_body_len(const xylem_http_res_t* res);

/**
 * @brief Free a response object.
 *
 * @param res  Response handle, or NULL (no-op).
 */
extern void xylem_http_res_destroy(xylem_http_res_t* res);

typedef struct {
    const char* username; /**< Basic auth username. */
    const char* password; /**< Basic auth password. */
} xylem_http_auth_t;

typedef struct {
    const char* host;     /**< Proxy hostname or IP. */
    uint16_t    port;     /**< Proxy port. */
    const char* username; /**< NULL = no auth. */
    const char* password; /**< NULL = no auth. */
} xylem_http_proxy_t;

typedef struct {
    uint64_t                    timeout_ms;    /**< Request timeout, 0 = default 30s. */
    int                         max_redirects; /**< 0 = no redirect following. */
    size_t                      max_body_size; /**< 0 = default 10 MiB. */
    const xylem_http_hdr_t*     headers;       /**< Custom request headers. */
    size_t                      header_count;  /**< Number of custom headers. */
    const char*                 range;         /**< Range header value, NULL = omit. */
    xylem_http_cookie_jar_t*    cookie_jar;    /**< NULL = no cookie management. */
    const xylem_http_proxy_t*   proxy;         /**< NULL = direct connection. */
    const xylem_http_auth_t*    auth;          /**< NULL = no authentication. */
    bool                        expect_continue; /**< Send Expect: 100-continue for bodies. */
} xylem_http_opts_t;

/**
 * @brief Send an HTTP GET request.
 *
 * Must be called from a coroutine. Suspends until response is received.
 *
 * @param url   Full URL (http:// scheme).
 * @param opts  Request options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_get(const char* url,
                                        const xylem_http_opts_t* opts);

/**
 * @brief Send an HTTP POST request.
 *
 * @param url           Full URL.
 * @param body          Request body, or NULL.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value.
 * @param opts          Request options, or NULL.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_post(const char* url,
                                         const void* body, size_t body_len,
                                         const char* content_type,
                                         const xylem_http_opts_t* opts);

/**
 * @brief Send an HTTP PUT request.
 *
 * @param url           Full URL.
 * @param body          Request body, or NULL.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value.
 * @param opts          Request options, or NULL.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_put(const char* url,
                                        const void* body, size_t body_len,
                                        const char* content_type,
                                        const xylem_http_opts_t* opts);

/**
 * @brief Send an HTTP DELETE request.
 *
 * @param url   Full URL.
 * @param opts  Request options, or NULL.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_delete(const char* url,
                                           const xylem_http_opts_t* opts);

/**
 * @brief Send an HTTP PATCH request.
 *
 * @param url           Full URL.
 * @param body          Request body, or NULL.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value.
 * @param opts          Request options, or NULL.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_patch(const char* url,
                                          const void* body, size_t body_len,
                                          const char* content_type,
                                          const xylem_http_opts_t* opts);

/**
 * @brief Create a request router.
 *
 * @return Router handle, or NULL on failure.
 */
extern xylem_http_router_t* xylem_http_router_create(void);

/**
 * @brief Destroy a router.
 *
 * @param r  Router handle, or NULL (no-op).
 */
extern void xylem_http_router_destroy(xylem_http_router_t* r);

/**
 * @brief Register a route.
 *
 * Pattern: "/exact", "/user/:id" (path param), "/static/ *" (wildcard).
 *
 * @param r         Router handle.
 * @param method    HTTP method, or NULL to match all.
 * @param pattern   URL path pattern.
 * @param handler   Request handler.
 * @param userdata  Passed to handler.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_router_add(xylem_http_router_t* r,
                                 const char* method,
                                 const char* pattern,
                                 xylem_http_handler_fn_t handler,
                                 void* userdata);

/**
 * @brief Register a global middleware.
 *
 * Middleware runs in registration order before the route handler.
 *
 * @param r         Router handle.
 * @param mw        Middleware function.
 * @param userdata  Passed to mw.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_router_use(xylem_http_router_t* r,
                                 xylem_http_middleware_fn_t mw,
                                 void* userdata);

/**
 * @brief Dispatch a request to the matching route.
 *
 * Sends 404 if no route matches.
 *
 * @param r    Router handle.
 * @param res  Response writer.
 * @param req  Parsed request.
 *
 * @return 0 if a route matched, -1 if 404 was sent.
 */
extern int xylem_http_router_dispatch(xylem_http_router_t* r,
                                      xylem_http_res_t* res,
                                      xylem_http_req_t* req);

typedef struct {
    const char* root;          /**< Filesystem root directory. */
    const char* index_file;    /**< Default document, NULL = "index.html". */
    int         max_age;       /**< Cache-Control max-age seconds, 0 = omit. */
    bool        precompressed; /**< Serve .gz files when client accepts gzip. */
} xylem_http_static_opts_t;

/**
 * @brief Register a static file handler on a router.
 *
 * @param r       Router handle.
 * @param prefix  URL prefix (e.g. "/static").
 * @param opts    Static file options.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_static_serve(xylem_http_router_t* r,
                                   const char* prefix,
                                   const xylem_http_static_opts_t* opts);

/**
 * @brief Create a cookie jar.
 *
 * @return Cookie jar handle, or NULL on failure.
 */
extern xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void);

/**
 * @brief Destroy a cookie jar.
 *
 * @param jar  Cookie jar handle, or NULL (no-op).
 */
extern void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar);

/**
 * @brief Percent-encode a string per RFC 3986.
 *
 * @param src      Source bytes.
 * @param src_len  Source length.
 * @param out_len  Output: encoded length.
 *
 * @return Allocated encoded string, or NULL. Caller frees.
 */
extern char* xylem_http_url_encode(const char* src, size_t src_len,
                                   size_t* out_len);

/**
 * @brief Decode a percent-encoded string.
 *
 * @param src      Source string.
 * @param src_len  Source length.
 * @param out_len  Output: decoded length.
 *
 * @return Allocated decoded string, or NULL. Caller frees.
 */
extern char* xylem_http_url_decode(const char* src, size_t src_len,
                                   size_t* out_len);

typedef struct {
    const char* allowed_origins;  /**< Comma-separated origins or "*". */
    const char* allowed_methods;  /**< Comma-separated methods. */
    const char* allowed_headers;  /**< Comma-separated headers. */
    const char* expose_headers;   /**< Headers to expose to client. */
    int         max_age;          /**< Preflight cache seconds, 0 = omit. */
    bool        allow_credentials; /**< Emit Allow-Credentials: true. */
} xylem_http_cors_t;

/**
 * @brief Generate CORS response headers.
 *
 * @param cors          CORS config, or NULL (returns 0).
 * @param origin        Request Origin header, or NULL (returns 0).
 * @param is_preflight  True for OPTIONS preflight requests.
 * @param out           Output array (at least 7 entries).
 * @param out_cap       Capacity of out array.
 *
 * @return Number of headers written.
 */
extern size_t xylem_http_cors_headers(const xylem_http_cors_t* cors,
                                      const char* origin,
                                      bool is_preflight,
                                      xylem_http_hdr_t* out,
                                      size_t out_cap);

/**
 * @brief Parse a multipart/form-data body.
 *
 * @param content_type  Content-Type header (must contain boundary).
 * @param body          Request body data.
 * @param body_len      Body length.
 *
 * @return Multipart handle, or NULL on error. Caller frees.
 */
extern xylem_http_multipart_t* xylem_http_multipart_parse(
    const char* content_type, const void* body, size_t body_len);

/**
 * @brief Get the number of parts.
 *
 * @param mp  Multipart handle.
 *
 * @return Part count, or 0 if mp is NULL.
 */
extern size_t xylem_http_multipart_count(const xylem_http_multipart_t* mp);

/**
 * @brief Get the name field of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Name string, or NULL.
 */
extern const char* xylem_http_multipart_name(const xylem_http_multipart_t* mp,
                                             size_t index);

/**
 * @brief Get the filename field of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Filename string, or NULL.
 */
extern const char* xylem_http_multipart_filename(
    const xylem_http_multipart_t* mp, size_t index);

/**
 * @brief Get the Content-Type of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Content-Type string, or NULL.
 */
extern const char* xylem_http_multipart_content_type(
    const xylem_http_multipart_t* mp, size_t index);

/**
 * @brief Get the body data of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Body data pointer, or NULL.
 */
extern const void* xylem_http_multipart_data(const xylem_http_multipart_t* mp,
                                             size_t index);

/**
 * @brief Get the body data length of a part.
 *
 * @param mp     Multipart handle.
 * @param index  Part index (0-based).
 *
 * @return Part body length.
 */
extern size_t xylem_http_multipart_data_len(const xylem_http_multipart_t* mp,
                                            size_t index);

/**
 * @brief Destroy a multipart handle.
 *
 * @param mp  Multipart handle, or NULL (no-op).
 */
extern void xylem_http_multipart_destroy(xylem_http_multipart_t* mp);

typedef struct xylem_http_multipart_builder_s xylem_http_multipart_builder_t;

/**
 * @brief Create a multipart form-data builder.
 *
 * @return Builder handle, or NULL on failure.
 */
extern xylem_http_multipart_builder_t* xylem_http_multipart_build_create(void);

/**
 * @brief Add a text field to the multipart body.
 *
 * @param b          Builder handle.
 * @param name       Field name (null-terminated).
 * @param value      Field value.
 * @param value_len  Value length in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_multipart_build_field(
    xylem_http_multipart_builder_t* b,
    const char* name,
    const void* value,
    size_t value_len);

/**
 * @brief Add a file part to the multipart body.
 *
 * @param b             Builder handle.
 * @param name          Field name (null-terminated).
 * @param filename      Filename for Content-Disposition.
 * @param content_type  MIME type, or NULL for application/octet-stream.
 * @param data          File data.
 * @param data_len      File data length.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_multipart_build_file(
    xylem_http_multipart_builder_t* b,
    const char* name,
    const char* filename,
    const char* content_type,
    const void* data,
    size_t data_len);

/**
 * @brief Finalize and serialize the multipart body.
 *
 * Produces a buffer suitable for xylem_http_post(). The caller must free
 * both *body and *content_type when done.
 *
 * @param b             Builder handle.
 * @param body          Output: malloc'd body buffer.
 * @param body_len      Output: body length.
 * @param content_type  Output: malloc'd Content-Type string with boundary.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_multipart_build_finish(
    xylem_http_multipart_builder_t* b,
    void** body,
    size_t* body_len,
    char** content_type);

/**
 * @brief Destroy a multipart builder.
 *
 * @param b  Builder handle, or NULL (no-op).
 */
extern void xylem_http_multipart_build_destroy(
    xylem_http_multipart_builder_t* b);

/**
 * @brief Build an SSE-formatted message string.
 *
 * @param event  Event type, or NULL for data-only.
 * @param data   Event data (must not be NULL).
 * @param len    Output: byte length of result, or NULL.
 *
 * @return Allocated SSE string, or NULL. Caller frees.
 */
extern char* xylem_http_sse_build(const char* event, const char* data,
                                  size_t* len);

/**
 * @brief Serve in-memory content with automatic ETag, If-None-Match, and Range.
 *
 * Handles conditional requests (304) and partial content (206) automatically.
 * Any handler can call this to get Go-style ServeContent behavior.
 *
 * @param res           Response writer.
 * @param req           Request (for Range/If-None-Match headers).
 * @param data          Content bytes.
 * @param data_len      Content length.
 * @param content_type  MIME type (e.g. "text/html").
 *
 * @note Caller is responsible for freeing data after this returns.
 */
extern void xylem_http_serve_content(xylem_http_res_t* res,
                                     xylem_http_req_t* req,
                                     const void* data,
                                     size_t data_len,
                                     const char* content_type);

/**
 * @brief Access log entry passed to the log callback.
 */
typedef struct {
    const char* method;      /**< Request method. */
    const char* path;        /**< Request path. */
    int         status;      /**< Response status code. */
    uint64_t    duration_ms; /**< Request handling duration in ms. */
} xylem_http_access_log_t;

/**
 * @brief Access log callback type.
 *
 * @param entry     Log entry.
 * @param userdata  Opaque pointer.
 */
typedef void (*xylem_http_access_log_fn_t)(
    const xylem_http_access_log_t* entry, void* userdata);

/**
 * @brief Create an access log handler wrapper.
 *
 * Returns a handler function that logs each request via log_fn after
 * calling the inner handler. Use the returned function as the server
 * handler and pass *out_ctx as its userdata.
 *
 * @param handler   The actual request handler to wrap.
 * @param userdata  Passed to handler.
 * @param log_fn    Log callback invoked after each request.
 * @param log_ud    Passed to log_fn.
 * @param out_ctx   Output: opaque context (caller must free when done).
 *
 * @return Handler function to pass to xylem_http_listen, or NULL on failure.
 */
extern xylem_http_handler_fn_t xylem_http_access_log_wrap(
    xylem_http_handler_fn_t    handler,
    void*                      userdata,
    xylem_http_access_log_fn_t log_fn,
    void*                      log_ud,
    void**                     out_ctx);
