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
 * HTTP Echo Server
 *
 * Demonstrates the xylem HTTP server API with a router:
 *   GET  /              -> "Hello, Xylem!"
 *   POST /echo          -> echoes the request body as JSON
 *   GET  /sse           -> sends 3 SSE events then closes
 *   GET  /chunked       -> sends a chunked response
 *   *    (no match)     -> 404 via router
 *
 * Usage: http-echo-server
 * Test:  http-echo-client  (or curl http://127.0.0.1:8080/)
 */

#include "xylem.h"
#include <stdio.h>
#include <string.h>

#define LISTEN_PORT 8080

static xylem_http_router_t* _router;

/* GET / */
static void _handle_index(xylem_http_conn_t* conn, xylem_http_req_t* req,
                           void* ud) {
    (void)req;
    (void)ud;
    const char* body = "Hello, Xylem!";
    xylem_http_conn_send(conn, 200, "text/plain", body, strlen(body),
                         NULL, 0);
}

/* POST /echo — echoes the request body back as JSON. */
static void _handle_echo(xylem_http_conn_t* conn, xylem_http_req_t* req,
                          void* ud) {
    (void)ud;
    const void* body     = xylem_http_req_body(req);
    size_t      body_len = xylem_http_req_body_len(req);

    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
                     "{\"method\":\"%s\",\"body\":\"%.*s\"}",
                     xylem_http_req_method(req),
                     (int)body_len, (const char*)body);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        xylem_http_conn_send(conn, 500, "text/plain",
                             "response too large", 18, NULL, 0);
        return;
    }
    xylem_http_conn_send(conn, 200, "application/json", buf, (size_t)n,
                         NULL, 0);
}

/* GET /sse — sends 3 Server-Sent Events then closes the stream. */
static void _handle_sse(xylem_http_conn_t* conn, xylem_http_req_t* req,
                         void* ud) {
    (void)req;
    (void)ud;
    xylem_http_conn_start_sse(conn, NULL, 0);
    xylem_http_conn_send_event(conn, "greeting", "hello");
    xylem_http_conn_send_event(conn, "greeting", "world");
    xylem_http_conn_send_sse_data(conn, "done");
    xylem_http_conn_end_sse(conn);
}

/* GET /chunked — sends a chunked response in 3 pieces. */
static void _handle_chunked(xylem_http_conn_t* conn, xylem_http_req_t* req,
                              void* ud) {
    (void)req;
    (void)ud;
    xylem_http_conn_start_chunked(conn, 200, "text/plain", NULL, 0);
    xylem_http_conn_send_chunk(conn, "chunk1 ", 7);
    xylem_http_conn_send_chunk(conn, "chunk2 ", 7);
    xylem_http_conn_send_chunk(conn, "chunk3",  6);
    xylem_http_conn_end_chunked(conn);
}

/* Dispatches requests through the router. */
static void _on_request(xylem_http_conn_t* conn, xylem_http_req_t* req,
                         void* ud) {
    (void)ud;
    xylem_http_router_dispatch(_router, conn, req);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_loop_t loop;
    xylem_loop_init(&loop);

    _router = xylem_http_router_create();
    xylem_http_router_add(_router, "GET",  "/",        _handle_index,   NULL);
    xylem_http_router_add(_router, "POST", "/echo",    _handle_echo,    NULL);
    xylem_http_router_add(_router, "GET",  "/sse",     _handle_sse,     NULL);
    xylem_http_router_add(_router, "GET",  "/chunked", _handle_chunked, NULL);

    xylem_http_srv_cfg_t cfg = {
        .host       = "127.0.0.1",
        .port       = LISTEN_PORT,
        .on_request = _on_request,
        .userdata   = NULL,
    };

    xylem_http_srv_t* srv = xylem_http_srv_create(&loop, &cfg);
    if (!srv) {
        xylem_loge("failed to create http server");
        return 1;
    }

    if (xylem_http_srv_start(srv) != 0) {
        xylem_loge("failed to start http server on port %d", LISTEN_PORT);
        xylem_http_srv_destroy(srv);
        return 1;
    }

    xylem_logi("http server listening on http://127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(&loop);

    xylem_http_srv_destroy(srv);
    xylem_http_router_destroy(_router);
    xylem_loop_deinit(&loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
