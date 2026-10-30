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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct http_transport_s {
    void* conn;
    int (*read)(void* conn, void* buf, int len);
    int (*write)(void* conn, const void* data, int len);
    void (*close)(void* conn);
    void (*set_rd_deadline)(void* conn, uint64_t ms);
    void (*set_wr_deadline)(void* conn, uint64_t ms);
    int (*remote_addr)(void* conn, char* host, size_t host_len, uint16_t* port);
    int (*local_addr)(void* conn, char* host, size_t host_len, uint16_t* port);
    int (*shutdown_wr)(void* conn); /* May be NULL (e.g. TLS). */
} http_transport_t;

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

typedef xylem_http_res_t http_res_t;

/**
 * @brief Perform a bounded HTTP/1.1 request with redirect handling.
 *
 * @param method        HTTP method.
 * @param url           Absolute HTTP or HTTPS URL.
 * @param body          Request bytes, or NULL for an empty body.
 * @param body_len      Number of request bytes.
 * @param content_type  Content-Type value, or NULL.
 * @param headers       Custom header array, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Options, or NULL for defaults.
 *
 * @return Response on success, NULL on failure; destroy with
 * xylem_http_res_destroy().
 */
extern xylem_http_res_t* http_do_request(
    const char*                  method,
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
extern void http_pool_clear(void);

/**
 * @brief Get the response status code.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param res  Response handle.
 *
 * @return HTTP status code, or 0 if res is NULL.
 */
extern int http_res_status(const xylem_http_res_t* res);

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
http_res_header(const xylem_http_res_t* res, const char* name);

/**
 * @brief Get the response body.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param res  Response handle.
 *
 * @return Body bytes, or NULL if no body.
 */
extern const void* http_res_body(const xylem_http_res_t* res);

/**
 * @brief Get the response body length.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param res  Response handle.
 *
 * @return Body length in bytes.
 */
extern size_t http_res_body_len(const xylem_http_res_t* res);

/**
 * @brief Free a response object.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param res  Response handle, or NULL (no-op).
 */
extern void http_res_destroy(xylem_http_res_t* res);
