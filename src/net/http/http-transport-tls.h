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
 * TLS HTTP transport factory.
 *
 * Builds an http_transport_t over a TLS connection for the server
 * (listen) and client (request) paths. The real implementation lives in
 * http-transport-tls.c when XYLEM_ENABLE_TLS is set; otherwise http-
 * transport-tls-stub.c provides stubs that return NULL. Not part of the
 * public API.
 */

_Pragma("once")

#include "http.h"

/**
 * @brief Start an HTTP server over TLS (HTTPS).
 *
 * @param host      Bind host.
 * @param port      Bind port.
 * @param handler   Request handler.
 * @param userdata  Opaque pointer passed to handler.
 * @param opts      Server options (opts->tls selects the certificate).
 *
 * @return Server handle, or NULL on failure (or always NULL in the stub).
 */
extern xylem_http_srv_t* http_tls_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts);

/**
 * @brief Perform an HTTPS client request.
 *
 * @param method        HTTP method string.
 * @param url           Full URL.
 * @param body          Request body, or NULL.
 * @param body_len      Body length in bytes.
 * @param content_type  Content-Type value, or NULL.
 * @param headers       Custom headers, or NULL.
 * @param header_count  Number of custom headers.
 * @param opts          Client options, or NULL.
 *
 * @return Response object, or NULL on failure (or always NULL in the stub).
 *         Caller frees via xylem_http_res_destroy().
 */
extern xylem_http_res_t* http_tls_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts);
