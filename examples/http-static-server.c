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
 * HTTP Static File Server
 *
 * Serves files from a directory with automatic MIME detection,
 * Range requests, conditional caching (304), and optional
 * pre-compressed .gz file support.
 *
 *   GET /static/*  -> files from ./public/
 *
 * Usage: http-static-server
 * Test:  curl http://127.0.0.1:8080/static/index.html
 *        curl -H "Range: bytes=0-99" http://127.0.0.1:8080/static/large.txt
 */

#include "xylem.h"
#include <stdio.h>
#include <string.h>

#define LISTEN_PORT 8080
#define STATIC_ROOT "public"

static xylem_http_router_t* _router;

/* Dispatches requests through the router. */
static void _on_request(xylem_http_writer_t* w, xylem_http_req_t* req,
                         void* ud) {
    (void)ud;
    xylem_http_router_dispatch(_router, w, req);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_loop_t* loop = xylem_loop_create();

    _router = xylem_http_router_create();

    xylem_http_static_opts_t opts = {
        .root          = STATIC_ROOT,
        .index_file    = "index.html",
        .max_age       = 3600,
        .precompressed = true,
    };
    xylem_http_static_serve(_router, "/static", &opts);

    xylem_http_srv_cfg_t cfg = {
        .host       = "127.0.0.1",
        .port       = LISTEN_PORT,
        .on_request = _on_request,
        .userdata   = NULL,
    };

    xylem_http_srv_t* srv = xylem_http_listen(loop, &cfg);
    if (!srv) {
        xylem_loge("failed to start http server on port %d", LISTEN_PORT);
        return 1;
    }

    xylem_logi("serving %s/ at http://127.0.0.1:%d/static/",
               STATIC_ROOT, LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_http_close_server(srv);
    xylem_http_router_destroy(_router);
    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
