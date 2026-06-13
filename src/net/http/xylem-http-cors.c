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

#include "xylem/net/http/xylem-http-cors.h"

#include "xylem/net/http/xylem-http-router.h"

#include "runtime/precond.h"

#include <stdio.h>
#include <string.h>

static const char* _default_methods = "GET, POST, HEAD";

static bool _origin_allowed(const xylem_http_cors_t* cfg, const char* origin) {
    if (!cfg->allowed_origins) {
        return true;
    }
    for (const char** p = cfg->allowed_origins; *p; p++) {
        if (strcmp(*p, "*") == 0 || strcmp(*p, origin) == 0) {
            return true;
        }
    }
    return false;
}

static void _join(const char** list, const char* fallback,
                  char* buf, size_t buf_len) {
    if (!list) {
        snprintf(buf, buf_len, "%s", fallback);
        return;
    }
    size_t off = 0;
    for (const char** p = list; *p && off < buf_len - 1; p++) {
        if (p != list) {
            off += (size_t)snprintf(buf + off, buf_len - off, ", ");
        }
        off += (size_t)snprintf(buf + off, buf_len - off, "%s", *p);
    }
}

static void _set_common_headers(xylem_http_res_t* res,
                                const xylem_http_cors_t* cfg,
                                const char* origin) {
    if (cfg->allowed_origins && cfg->allowed_origins[0] &&
        strcmp(cfg->allowed_origins[0], "*") == 0 && !cfg->allow_credentials) {
        xylem_http_res_set_header(res, "Access-Control-Allow-Origin", "*");
    } else {
        xylem_http_res_set_header(res, "Access-Control-Allow-Origin", origin);
        xylem_http_res_set_header(res, "Vary", "Origin");
    }

    if (cfg->allow_credentials) {
        xylem_http_res_set_header(
            res, "Access-Control-Allow-Credentials", "true");
    }

    if (cfg->exposed_headers) {
        char buf[1024];
        _join(cfg->exposed_headers, "", buf, sizeof(buf));
        if (buf[0]) {
            xylem_http_res_set_header(
                res, "Access-Control-Expose-Headers", buf);
        }
    }
}

void xylem_http_cors_middleware(xylem_http_res_t* res,
                               xylem_http_req_t* req,
                               void*             userdata) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_cors_middleware");

    const xylem_http_cors_t* cfg = (const xylem_http_cors_t*)userdata;
    const char* origin = xylem_http_req_header(req, "Origin");

    if (!origin || !origin[0]) {
        xylem_http_router_next(res, req);
        return;
    }

    if (!_origin_allowed(cfg, origin)) {
        xylem_http_router_next(res, req);
        return;
    }

    const char* method = xylem_http_req_method(req);
    if (strcmp(method, "OPTIONS") == 0) {
        /* preflight */
        _set_common_headers(res, cfg, origin);

        char buf[1024];
        _join(cfg->allowed_methods, _default_methods, buf, sizeof(buf));
        xylem_http_res_set_header(
            res, "Access-Control-Allow-Methods", buf);

        const char* req_headers =
            xylem_http_req_header(req, "Access-Control-Request-Headers");
        if (cfg->allowed_headers) {
            _join(cfg->allowed_headers, "", buf, sizeof(buf));
            xylem_http_res_set_header(
                res, "Access-Control-Allow-Headers", buf);
        } else if (req_headers) {
            /* reflect requested headers */
            xylem_http_res_set_header(
                res, "Access-Control-Allow-Headers", req_headers);
        }

        if (cfg->max_age > 0) {
            char age_buf[16];
            snprintf(age_buf, sizeof(age_buf), "%d", cfg->max_age);
            xylem_http_res_set_header(
                res, "Access-Control-Max-Age", age_buf);
        }

        xylem_http_res_set_status(res, 204);
        xylem_http_res_write(res, NULL, 0);
        /* no next() -- short-circuit */
        return;
    }

    /* normal cross-origin request */
    _set_common_headers(res, cfg, origin);
    xylem_http_router_next(res, req);
}
