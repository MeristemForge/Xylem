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
 * HTTPS Static File Server
 *
 * Same as http-static-server but served over TLS.
 * If cert.pem / key.pem are missing, generates a self-signed
 * certificate automatically via the openssl command-line tool.
 *
 *   GET /static/(wildcard)  -> files from ./public/
 *
 * Usage: https-static-server
 * Test:  curl -k https://127.0.0.1:8443/static/index.html
 */

#include "xylem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LISTEN_PORT 8443
#define STATIC_ROOT "public"
#define CERT_FILE   "cert.pem"
#define KEY_FILE    "key.pem"

static xylem_http_router_t* _router;

static int _ensure_cert(void) {
    FILE* f = fopen(CERT_FILE, "r");
    if (f) {
        fclose(f);
        return 0;
    }

    xylem_logi("generating self-signed certificate...");

    const char* cnf = "_xylem_tmp.cnf";
    f = fopen(cnf, "w");
    if (!f) {
        return -1;
    }
    fprintf(f,
            "[req]\n"
            "distinguished_name=dn\n"
            "prompt=no\n"
            "[dn]\n"
            "CN=localhost\n");
    fclose(f);

    int rc = system(
        "openssl req -x509 -newkey rsa:2048"
        " -keyout " KEY_FILE " -out " CERT_FILE
        " -days 365 -nodes"
        " -config _xylem_tmp.cnf");
    remove(cnf);
    return (rc == 0) ? 0 : -1;
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

    if (_ensure_cert() != 0) {
        xylem_loge("failed to generate certificate (is openssl installed?)");
        return 1;
    }

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
        .tls_cert   = CERT_FILE,
        .tls_key    = KEY_FILE,
    };

    xylem_http_srv_t* srv = xylem_http_listen(loop, &cfg);
    if (!srv) {
        xylem_loge("failed to start https server on port %d", LISTEN_PORT);
        return 1;
    }

    xylem_logi("serving %s/ at https://127.0.0.1:%d/static/",
               STATIC_ROOT, LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_http_close_server(srv);
    xylem_http_router_destroy(_router);
    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
