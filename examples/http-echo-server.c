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
 *   Middleware: request logger, simple auth check
 *   GET  /              -> "Hello, Xylem!"
 *   POST /echo          -> echoes the request body as JSON
 *   GET  /sse           -> sends 3 SSE events then closes
 *   GET  /chunked       -> sends a chunked response via write()
 *   GET  /writer        -> Go-style writer mode demo
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

/* Log every request: method + url. */
static int _log_middleware(xylem_http_writer_t* w, xylem_http_req_t* req,
                           void* ud) {
    (void)w;
    (void)ud;
    xylem_logi("[req] %s %s", xylem_http_req_method(req),
               xylem_http_req_url(req));
    return 0;
}

/**
 * Reject requests without a valid Authorization header.
 * Accepts "Bearer xylem-demo-token" for demonstration purposes.
 */
static int _auth_middleware(xylem_http_writer_t* w, xylem_http_req_t* req,
                            void* ud) {
    (void)ud;
    const char* auth = xylem_http_req_header(req, "Authorization");
    if (auth && strcmp(auth, "Bearer xylem-demo-token") == 0) {
        return 0;
    }
    xylem_http_writer_set_status(w, 401);
    xylem_http_writer_set_header(w, "Content-Type", "application/json");
    const char* body = "{\"error\":\"unauthorized\"}";
    xylem_http_writer_write(w, body, strlen(body));
    return -1;
}

/* GET / */
static void _handle_index(xylem_http_writer_t* w, xylem_http_req_t* req,
                           void* ud) {
    (void)req;
    (void)ud;
    xylem_http_writer_set_header(w, "Content-Type", "text/plain");
    const char* body = "Hello, Xylem!";
    xylem_http_writer_write(w, body, strlen(body));
}

/* POST /echo -- echoes the request body back as JSON. */
static void _handle_echo(xylem_http_writer_t* w, xylem_http_req_t* req,
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
        xylem_http_writer_set_status(w, 500);
        xylem_http_writer_set_header(w, "Content-Type", "text/plain");
        xylem_http_writer_write(w, "response too large", 18);
        return;
    }
    xylem_http_writer_set_header(w, "Content-Type", "application/json");
    xylem_http_writer_write(w, buf, (size_t)n);
}

/* GET /sse -- sends 3 Server-Sent Events then closes the stream. */
static void _handle_sse(xylem_http_writer_t* w, xylem_http_req_t* req,
                         void* ud) {
    (void)req;
    (void)ud;
    xylem_http_writer_set_header(w, "Content-Type", "text/event-stream");
    xylem_http_writer_set_header(w, "Cache-Control", "no-cache");
    xylem_http_writer_set_header(w, "Connection", "keep-alive");

    size_t len;
    char* msg;

    msg = xylem_http_sse_build("greeting", "hello", &len);
    xylem_http_writer_write(w, msg, len);
    free(msg);

    msg = xylem_http_sse_build("greeting", "world", &len);
    xylem_http_writer_write(w, msg, len);
    free(msg);

    msg = xylem_http_sse_build(NULL, "done", &len);
    xylem_http_writer_write(w, msg, len);
    free(msg);
}

/* GET /chunked -- sends a chunked response in 3 pieces via write(). */
static void _handle_chunked(xylem_http_writer_t* w, xylem_http_req_t* req,
                              void* ud) {
    (void)req;
    (void)ud;
    xylem_http_writer_set_header(w, "Content-Type", "text/plain");
    xylem_http_writer_write(w, "chunk1 ", 7);
    xylem_http_writer_write(w, "chunk2 ", 7);
    xylem_http_writer_write(w, "chunk3",  6);
}

/* GET /writer -- Go-style writer mode: set headers, then stream body. */
static void _handle_writer(xylem_http_writer_t* w, xylem_http_req_t* req,
                            void* ud) {
    (void)req;
    (void)ud;
    xylem_http_writer_set_header(w, "Content-Type", "application/json");
    xylem_http_writer_set_header(w, "X-Custom", "xylem-writer");

    const char* p1 = "{\"parts\":[";
    xylem_http_writer_write(w, p1, strlen(p1));

    const char* p2 = "\"one\",\"two\",\"three\"";
    xylem_http_writer_write(w, p2, strlen(p2));

    const char* p3 = "]}";
    xylem_http_writer_write(w, p3, strlen(p3));
}

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

    /* Register middleware (runs in order before route handlers). */
    xylem_http_router_use(_router, _log_middleware, NULL);
    xylem_http_router_use(_router, _auth_middleware, NULL);

    xylem_http_router_add(_router, "GET",  "/",        _handle_index,   NULL);
    xylem_http_router_add(_router, "POST", "/echo",    _handle_echo,    NULL);
    xylem_http_router_add(_router, "GET",  "/sse",     _handle_sse,     NULL);
    xylem_http_router_add(_router, "GET",  "/chunked", _handle_chunked, NULL);
    xylem_http_router_add(_router, "GET",  "/writer",  _handle_writer,  NULL);

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

    xylem_logi("http server listening on http://127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_http_close_server(srv);
    xylem_http_router_destroy(_router);
    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
