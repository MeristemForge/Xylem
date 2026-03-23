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

/* Opaque types */
typedef struct xylem_http_res_s        xylem_http_res_t;
typedef struct xylem_http_cookie_jar_s xylem_http_cookie_jar_t;

/**
 * @brief Per-request options for the HTTP client.
 *
 * Pass NULL to any request function to use defaults.
 * Zero-initialized fields use their default values:
 *   timeout_ms = 30000, max_redirects = 0, max_body_size = 10 MiB,
 *   headers = NULL, header_count = 0.
 */
typedef struct {
    uint64_t                    timeout_ms;
    int                         max_redirects;
    size_t                      max_body_size;
    const xylem_http_hdr_t*     headers;      /**< Custom request headers, NULL for none. */
    size_t                      header_count;  /**< Number of custom request headers. */
    xylem_http_cookie_jar_t*    cookie_jar;    /**< Cookie jar for automatic cookie management, NULL to disable. */
    const char*                 range;         /**< Range header value (e.g. "bytes=0-499"), NULL to omit. */
} xylem_http_cli_opts_t;

/**
 * @brief Send a synchronous HTTP GET request.
 *
 * Creates a temporary event loop internally, resolves the host,
 * connects via TCP or TLS based on the URL scheme, sends the
 * request, and blocks until the response is received.
 *
 * @param url   Full URL string (e.g. "http://example.com/path").
 * @param opts  Per-request options, or NULL for defaults.
 *
 * @return Response handle on success, NULL on any error.
 *
 * @note The caller must free the response with xylem_http_res_destroy().
 */
extern xylem_http_res_t* xylem_http_cli_get(const char* url,
                                                 const xylem_http_cli_opts_t* opts);

/**
 * @brief Send a synchronous HTTP POST request.
 *
 * @param url           Full URL string.
 * @param body          Request body, or NULL for empty body.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value.
 * @param opts          Per-request options, or NULL for defaults.
 *
 * @return Response handle on success, NULL on any error.
 *
 * @note The caller must free the response with xylem_http_res_destroy().
 */
extern xylem_http_res_t* xylem_http_cli_post(const char* url,
                                                  const void* body,
                                                  size_t body_len,
                                                  const char* content_type,
                                                  const xylem_http_cli_opts_t* opts);

/**
 * @brief Send a synchronous HTTP PUT request.
 *
 * @param url           Full URL string.
 * @param body          Request body, or NULL for empty body.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value.
 * @param opts          Per-request options, or NULL for defaults.
 *
 * @return Response handle on success, NULL on any error.
 *
 * @note The caller must free the response with xylem_http_res_destroy().
 */
extern xylem_http_res_t* xylem_http_cli_put(const char* url,
                                                 const void* body,
                                                 size_t body_len,
                                                 const char* content_type,
                                                 const xylem_http_cli_opts_t* opts);

/**
 * @brief Send a synchronous HTTP DELETE request.
 *
 * @param url   Full URL string.
 * @param opts  Per-request options, or NULL for defaults.
 *
 * @return Response handle on success, NULL on any error.
 *
 * @note The caller must free the response with xylem_http_res_destroy().
 */
extern xylem_http_res_t* xylem_http_cli_delete(const char* url,
                                                    const xylem_http_cli_opts_t* opts);

/**
 * @brief Send a synchronous HTTP PATCH request.
 *
 * @param url           Full URL string.
 * @param body          Request body, or NULL for empty body.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type header value.
 * @param opts          Per-request options, or NULL for defaults.
 *
 * @return Response handle on success, NULL on any error.
 *
 * @note The caller must free the response with xylem_http_res_destroy().
 */
extern xylem_http_res_t* xylem_http_cli_patch(const char* url,
                                                   const void* body,
                                                   size_t body_len,
                                                   const char* content_type,
                                                   const xylem_http_cli_opts_t* opts);

/**
 * @brief Get the HTTP status code from a response.
 *
 * @param res  Response handle.
 *
 * @return HTTP status code (e.g. 200, 404).
 */
extern int xylem_http_res_status(const xylem_http_res_t* res);

/**
 * @brief Get a response header value by name.
 *
 * Performs case-insensitive ASCII matching per RFC 7230.
 *
 * @param res   Response handle.
 * @param name  Header name to look up.
 *
 * @return Header value string, or NULL if not found.
 *
 * @note The returned pointer is valid until xylem_http_res_destroy().
 */
extern const char* xylem_http_res_header(const xylem_http_res_t* res,
                                              const char* name);

/**
 * @brief Get the response body data.
 *
 * @param res  Response handle.
 *
 * @return Pointer to body bytes, or NULL if no body.
 *
 * @note The returned pointer is valid until xylem_http_res_destroy().
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
 * @brief Create a cookie jar for automatic cookie management.
 *
 * The jar stores cookies received via Set-Cookie response headers
 * and automatically attaches matching cookies to subsequent requests
 * when passed through xylem_http_cli_opts_t.cookie_jar.
 *
 * @return Cookie jar handle on success, NULL on allocation failure.
 *
 * @note The caller must free the jar with xylem_http_cookie_jar_destroy().
 */
extern xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void);

/**
 * @brief Destroy a cookie jar and free all stored cookies.
 *
 * @param jar  Cookie jar handle, or NULL (no-op).
 */
extern void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar);

/**
 * @brief Destroy a response and free all associated memory.
 *
 * @param res  Response handle, or NULL (no-op).
 */
extern void xylem_http_res_destroy(xylem_http_res_t* res);
