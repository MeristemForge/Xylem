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

typedef struct xylem_http_cookie_jar_s xylem_http_cookie_jar_t;

typedef struct xylem_http_hdr_s {
    const char* name;
    const char* value;
} xylem_http_hdr_t;

typedef struct xylem_http_req_s xylem_http_req_t;
typedef struct xylem_http_res_s xylem_http_res_t;
typedef struct xylem_http_srv_s xylem_http_srv_t;

typedef void (*xylem_http_handler_fn_t)(
    xylem_http_res_t* res,
    xylem_http_req_t* req,
    void*             userdata);

typedef struct xylem_http_tls_s {
    const char* cert;        /* PEM certificate path. */
    const char* key;         /* PEM private key path. */
    const char* ca;          /* CA path, NULL = system default. */
    bool        skip_verify; /* true = skip peer cert verification. */
} xylem_http_tls_t;

typedef struct xylem_http_srv_opts_s {
    uint64_t                idle_timeout_ms;        /* 0 = default 60000 ms. */
    uint64_t                read_header_timeout_ms; /* 0 = default 10000 ms. */
    uint64_t                write_timeout_ms;       /* 0 = default 30000 ms. */
    xylem_http_handler_fn_t on_upgrade;             /* Upgrade handler, NULL = reject 501. */
    void*                   upgrade_userdata;       /* Passed to on_upgrade. */
    const xylem_http_tls_t* tls;                   /* Non-NULL = HTTPS server. */
} xylem_http_srv_opts_t;

typedef struct xylem_http_proxy_s {
    const char* host;
    uint16_t    port;
    const char* username; /* NULL = no auth. */
    const char* password; /* NULL = no auth. */
} xylem_http_proxy_t;

typedef struct xylem_http_cli_opts_s {
    uint64_t                  timeout_ms;    /* 0 = no timeout. */
    int                       max_redirects; /* 0 = disable, default 10. */
    const xylem_http_proxy_t* proxy;         /* NULL = from env (HTTPS only). */
    const xylem_http_tls_t*   tls;           /* Non-NULL = custom TLS config. */
} xylem_http_cli_opts_t;

/**
 * @brief Start an HTTP server.
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
 * @param srv         Server handle.
 * @param timeout_ms  Maximum wait time, 0 = immediate close.
 *
 * @return 0 on clean shutdown, -1 on timeout.
 */
extern int xylem_http_shutdown(xylem_http_srv_t* srv, uint64_t timeout_ms);

/**
 * @brief Get the server listening address.
 *
 * @param srv       Server handle.
 * @param host      Output buffer for host string, or NULL to skip.
 * @param host_len  Size of host buffer.
 * @param port      Output port, or NULL to skip.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_http_srv_addr(
    xylem_http_srv_t* srv,
    char*             host,
    size_t            host_len,
    uint16_t*         port);

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
 * @brief Get all request headers.
 *
 * @param req      Request handle.
 * @param headers  Output: pointer to header array.
 * @param count    Output: number of headers.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_http_req_headers(
    const xylem_http_req_t*  req,
    const xylem_http_hdr_t** headers,
    size_t*                  count);

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
 * @brief Get the remote address of the client.
 *
 * @param req       Request handle.
 * @param host      Output buffer for IP string, or NULL to skip.
 * @param host_len  Size of host buffer.
 * @param port      Output port, or NULL to skip.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_http_req_remote_addr(
    const xylem_http_req_t* req,
    char*                   host,
    size_t                  host_len,
    uint16_t*               port);

/**
 * @brief Set the response status code.
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
 * @param res   Response handle.
 * @param data  Body data.
 * @param len   Data length in bytes.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_http_res_write(xylem_http_res_t* res,
                                const void* data, size_t len);

/**
 * @brief Accept an HTTP Upgrade request.
 *
 * @param res        Response handle.
 * @param transport  Output: underlying connection handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_res_upgrade(xylem_http_res_t* res, void** transport);

/**
 * @brief Detach the underlying connection without sending any response.
 *
 * Unlike xylem_http_res_upgrade(), this writes no status line or headers:
 * the engine simply relinquishes the connection and the caller owns every
 * subsequent byte. Use it to implement CONNECT-style proxying, where the
 * response (200 Connection Established vs. 502) must be decided only after
 * dialing the upstream, or any custom protocol takeover.
 *
 * Must be called before any header or body byte has been sent. After a
 * successful call the response handle no longer owns the connection;
 * the caller is responsible for reading, writing, and closing it.
 *
 * @param res        Response handle.
 * @param transport  Output: underlying connection handle (caller owns it).
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_http_res_hijack(xylem_http_res_t* res, void** transport);

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

/**
 * @brief Send an HTTP request with an arbitrary method.
 *
 * @param method        HTTP method string (e.g. "OPTIONS", "PROPFIND").
 * @param url           Full URL.
 * @param body          Request body, or NULL.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value, or NULL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 * @param jar           Cookie jar, or NULL to disable cookie handling.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts,
    xylem_http_cookie_jar_t*     jar);

/**
 * @brief Send an HTTP GET request.
 *
 * @param url           Full URL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_get(const char* url,
                                        const xylem_http_hdr_t* headers,
                                        size_t header_count,
                                        const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP HEAD request.
 *
 * @param url           Full URL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_head(const char* url,
                                         const xylem_http_hdr_t* headers,
                                         size_t header_count,
                                         const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP POST request.
 *
 * @param url           Full URL.
 * @param body          Request body, or NULL.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value, or NULL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_post(const char* url,
                                         const void* body, size_t body_len,
                                         const char* content_type,
                                         const xylem_http_hdr_t* headers,
                                         size_t header_count,
                                         const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP PUT request.
 *
 * @param url           Full URL.
 * @param body          Request body, or NULL.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value, or NULL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_put(const char* url,
                                        const void* body, size_t body_len,
                                        const char* content_type,
                                        const xylem_http_hdr_t* headers,
                                        size_t header_count,
                                        const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP DELETE request.
 *
 * @param url           Full URL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_delete(const char* url,
                                           const xylem_http_hdr_t* headers,
                                           size_t header_count,
                                           const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP PATCH request.
 *
 * @param url           Full URL.
 * @param body          Request body, or NULL.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value, or NULL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_patch(const char* url,
                                          const void* body, size_t body_len,
                                          const char* content_type,
                                          const xylem_http_hdr_t* headers,
                                          size_t header_count,
                                          const xylem_http_cli_opts_t* opts);

/**
 * @brief Serve in-memory content with automatic ETag, If-None-Match, and Range.
 *
 * @param res           Response writer.
 * @param req           Request (for Range/If-None-Match headers).
 * @param data          Content bytes.
 * @param data_len      Content length.
 * @param content_type  MIME type (e.g. "text/html").
 */
extern void xylem_http_serve_content(xylem_http_res_t* res,
                                     xylem_http_req_t* req,
                                     const void* data,
                                     size_t data_len,
                                     const char* content_type);
