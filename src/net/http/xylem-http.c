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

#include "xylem/net/http/xylem-http.h"

#include "http-utils.h"
#include "http.h"
#include "runtime/precond.h"

#include <stdlib.h>

xylem_http_res_t* xylem_http_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_request");

    return http_do_request(
        method,
        url,
        body,
        body_len,
        content_type,
        headers,
        header_count,
        opts);
}

xylem_http_res_t* xylem_http_get(
    const char*                  url,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_get");

    return http_do_request(
        "GET",
        url,
        NULL,
        0,
        NULL,
        headers,
        header_count,
        opts);
}

xylem_http_res_t* xylem_http_head(
    const char*                  url,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_head");

    return http_do_request(
        "HEAD",
        url,
        NULL,
        0,
        NULL,
        headers,
        header_count,
        opts);
}

xylem_http_res_t* xylem_http_post(
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_post");

    return http_do_request(
        "POST",
        url,
        body,
        body_len,
        content_type,
        headers,
        header_count,
        opts);
}

xylem_http_res_t* xylem_http_put(
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_put");

    return http_do_request(
        "PUT",
        url,
        body,
        body_len,
        content_type,
        headers,
        header_count,
        opts);
}

xylem_http_res_t* xylem_http_delete(
    const char*                  url,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_delete");

    return http_do_request(
        "DELETE",
        url,
        NULL,
        0,
        NULL,
        headers,
        header_count,
        opts);
}

xylem_http_res_t* xylem_http_patch(
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_patch");

    return http_do_request(
        "PATCH",
        url,
        body,
        body_len,
        content_type,
        headers,
        header_count,
        opts);
}

void xylem_http_pool_clear(void) {
    http_pool_clear();
}

int xylem_http_res_status(const xylem_http_res_t* res) {
    return http_res_status(res);
}

const char*
xylem_http_res_header(const xylem_http_res_t* res, const char* name) {
    return http_res_header(res, name);
}

const void* xylem_http_res_body(const xylem_http_res_t* res) {
    return http_res_body(res);
}

size_t xylem_http_res_body_len(const xylem_http_res_t* res) {
    return http_res_body_len(res);
}

void xylem_http_res_destroy(xylem_http_res_t* res) {
    http_res_destroy(res);
}
