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

#include <stdbool.h>

typedef struct {
    const char** allowed_origins; /* NULL-terminated, NULL = wildcard "*". */
    const char** allowed_methods; /* NULL-terminated, NULL = "GET,POST,HEAD". */
    const char** allowed_headers; /* NULL-terminated, NULL = allow any. */
    const char** exposed_headers; /* NULL-terminated, NULL = none. */
    int          max_age;         /* Preflight cache seconds, 0 = omit header. */
    bool         allow_credentials;
} xylem_http_cors_t;

/**
 * @brief CORS middleware handler.
 *
 * @note [COROUTINE-ONLY]
 *
 * Pass a pointer to xylem_http_cors_t as userdata when registering:
 *   xylem_http_router_use(router, xylem_http_cors_middleware, &cors_cfg);
 *
 * Handles OPTIONS preflight (responds 204, short-circuits) and injects
 * Access-Control-* headers on normal requests before calling next.
 */
extern void xylem_http_cors_middleware(xylem_http_res_t* res,
                                      xylem_http_req_t* req,
                                      void*             userdata);
