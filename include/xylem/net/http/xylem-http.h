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

typedef struct xylem_http_hdr_s {
    const char* name;
    const char* value;
} xylem_http_hdr_t;

typedef struct xylem_http_res_s xylem_http_res_t;

typedef struct xylem_http_tls_s {
    const char* cert;        /* PEM client certificate path (optional mTLS). */
    const char* key;         /* PEM client private key path. */
    const char* ca;          /* CA path, NULL = system default. */
    bool        skip_verify; /* true = skip peer cert verification. */
} xylem_http_tls_t;

typedef struct xylem_http_proxy_s {
    const char* host;
    uint16_t    port;
    const char* username; /* NULL = no auth. */
    const char* password; /* NULL = no auth. */
} xylem_http_proxy_t;

typedef struct xylem_http_cli_opts_s {
    /**
     * Total request budget, including redirects; 0 disables the limit.
     */
    uint64_t timeout_ms;
    int32_t  max_redirects; /* NULL opts select 10; 0 disables redirects. */
    /**
     * NULL selects http_proxy/https_proxy and no_proxy from the environment.
     * A descriptor with a NULL host bypasses environment proxy selection.
     */
    const xylem_http_proxy_t* proxy;
    const xylem_http_tls_t*   tls; /* NULL selects verified system trust. */
    /**
     * Bounds both encoded and gzip-decoded bytes; 0 selects 16 MiB.
     */
    size_t max_response_body;
    bool   expect_continue; /* Opt in to Expect: 100-continue. */
    /**
     * False rejects HTTPS-to-HTTP redirects to prevent silent downgrades.
     */
    bool allow_https_downgrade;
} xylem_http_cli_opts_t;

/**
 * @brief Get the response status code.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param res  Response handle.
 *
 * @return HTTP status code, or 0 if res is NULL.
 */
extern int xylem_http_res_status(const xylem_http_res_t* res);

/**
 * @brief Get a response header value by name (case-insensitive).
 *
 * @note [COROUTINE-ONLY]
 *
 * @param res   Response handle.
 * @param name  Header name.
 *
 * @return Header value, or NULL if not found.
 */
extern const char*
xylem_http_res_header(const xylem_http_res_t* res, const char* name);

/**
 * @brief Get the response body.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param res  Response handle.
 *
 * @return Body bytes, or NULL if no body.
 */
extern const void* xylem_http_res_body(const xylem_http_res_t* res);

/**
 * @brief Get the response body length.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param res  Response handle.
 *
 * @return Body length in bytes.
 */
extern size_t xylem_http_res_body_len(const xylem_http_res_t* res);

/**
 * @brief Free a response object.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param res  Response handle, or NULL (no-op).
 */
extern void xylem_http_res_destroy(xylem_http_res_t* res);

/**
 * @brief Send an HTTP request with an arbitrary method.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param method        HTTP method string (e.g. "OPTIONS", "PROPFIND").
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
extern xylem_http_res_t* xylem_http_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP GET request.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param url           Full URL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_get(
    const char*                  url,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP HEAD request.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param url           Full URL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_head(
    const char*                  url,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP POST request.
 *
 * @note [COROUTINE-ONLY]
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
extern xylem_http_res_t* xylem_http_post(
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP PUT request.
 *
 * @note [COROUTINE-ONLY]
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
extern xylem_http_res_t* xylem_http_put(
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP DELETE request.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param url           Full URL.
 * @param headers       Custom request headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure. Caller frees.
 */
extern xylem_http_res_t* xylem_http_delete(
    const char*                  url,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

/**
 * @brief Send an HTTP PATCH request.
 *
 * @note [COROUTINE-ONLY]
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
extern xylem_http_res_t* xylem_http_patch(
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);

/**
 * @brief Release all idle HTTP/HTTPS connections and pool storage.
 * @note [COROUTINE-ONLY] Call after all requests have returned, before
 * shutting down the runtime. Must not race with requests or another clear.
 * A subsequent request creates a new pool.
 */
extern void xylem_http_pool_clear(void);
