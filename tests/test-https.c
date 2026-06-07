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
 * HTTPS integration tests: exercise the TLS HTTP transport
 * (http-transport-tls.c, http_tls_listen / http_tls_request) end-to-end.
 * test-tls.c covers the raw TLS engine; this file covers its integration
 * into the HTTP server/client stack: certificate-backed HTTPS listener,
 * https:// scheme dispatch, peer verification (skip + pinned CA), connection
 * pooling and request bodies over TLS.
 */

#include "xylem.h"
#include "assert.h"
#define TEST_WITH_TLS
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helpers. */

#define HTTPS_CERT "test_https_cert.pem"
#define HTTPS_KEY  "test_https_key.pem"

static uint16_t _srv_port(xylem_http_srv_t* srv) {
    uint16_t port = 0;
    xylem_http_srv_addr(srv, NULL, 0, &port);
    return port;
}

static void _hello_handler(xylem_http_res_t* res,
                           xylem_http_req_t* req,
                           void* userdata) {
    (void)req;
    (void)userdata;
    xylem_http_res_set_status(res, 200);
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    xylem_http_res_write(res, "hello", 5);
}

/* Echoes the request body back (exercises larger writes over TLS). */
static void _echo_handler(xylem_http_res_t* res,
                          xylem_http_req_t* req,
                          void* userdata) {
    (void)userdata;
    const void* body = xylem_http_req_body(req);
    size_t      len  = xylem_http_req_body_len(req);
    xylem_http_res_set_status(res, 200);
    xylem_http_res_set_header(res, "Content-Type", "application/octet-stream");
    xylem_http_res_write(res, body, len);
}

/* Echoes the request path (used by the pool-reuse test). */
static void _path_handler(xylem_http_res_t* res,
                          xylem_http_req_t* req,
                          void* userdata) {
    (void)userdata;
    const char* url = xylem_http_req_url(req);
    xylem_http_res_set_status(res, 200);
    xylem_http_res_write(res, url, strlen(url));
}

static xylem_http_srv_t* _listen_tls(xylem_http_handler_fn_t handler) {
    static const xylem_http_tls_t tls = {
        .cert = HTTPS_CERT,
        .key  = HTTPS_KEY,
    };
    xylem_http_srv_opts_t opts = {0};
    opts.tls = &tls;
    return xylem_http_listen("127.0.0.1", 0, handler, NULL, &opts);
}

/* Test: basic HTTPS GET (skip_verify client). */

static void _test_get_main(void* arg) {
    (void)arg;
    ASSERT(_cert_gen(HTTPS_CERT, HTTPS_KEY) == 0);

    xylem_http_srv_t* srv = _listen_tls(_hello_handler);
    ASSERT(srv != NULL);
    uint16_t port = _srv_port(srv);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "https://127.0.0.1:%u/test", (unsigned)port);

    xylem_http_tls_t cli_tls = { .skip_verify = true };
    xylem_http_cli_opts_t opts = {0};
    opts.tls = &cli_tls;

    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 5);
    ASSERT(memcmp(xylem_http_res_body(res), "hello", 5) == 0);

    const char* ct = xylem_http_res_header(res, "Content-Type");
    ASSERT(ct != NULL);
    ASSERT(strcmp(ct, "text/plain") == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    remove(HTTPS_CERT);
    remove(HTTPS_KEY);
    xylem_shutdown();
}

static void test_https_get(void) {
    xylem_run(_test_get_main, NULL, NULL);
}

/* Test: pinned-CA verification (real cert checking path). */

static void _test_pinned_ca_main(void* arg) {
    (void)arg;
    ASSERT(_cert_gen(HTTPS_CERT, HTTPS_KEY) == 0);

    xylem_http_srv_t* srv = _listen_tls(_hello_handler);
    ASSERT(srv != NULL);
    uint16_t port = _srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "https://127.0.0.1:%u/", (unsigned)port);

    /**
     * Pin the self-signed cert as the trusted CA: verification is ON and
     * must succeed because the SAN contains IP:127.0.0.1.
     */
    xylem_http_tls_t cli_tls = { .ca = HTTPS_CERT };
    xylem_http_cli_opts_t opts = {0};
    opts.tls = &cli_tls;

    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(memcmp(xylem_http_res_body(res), "hello", 5) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    remove(HTTPS_CERT);
    remove(HTTPS_KEY);
    xylem_shutdown();
}

static void test_https_pinned_ca(void) {
    xylem_run(_test_pinned_ca_main, NULL, NULL);
}

/* Test: verification failure on untrusted self-signed cert. */

static void _test_verify_fail_main(void* arg) {
    (void)arg;
    ASSERT(_cert_gen(HTTPS_CERT, HTTPS_KEY) == 0);

    xylem_http_srv_t* srv = _listen_tls(_hello_handler);
    ASSERT(srv != NULL);
    uint16_t port = _srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "https://127.0.0.1:%u/", (unsigned)port);

    /**
     * Default TLS (system trust, verification ON, no pinned CA): the
     * self-signed cert is untrusted, so the request must fail.
     */
    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(res == NULL);

    xylem_http_close(srv);
    remove(HTTPS_CERT);
    remove(HTTPS_KEY);
    xylem_shutdown();
}

static void test_https_verify_fail(void) {
    xylem_run(_test_verify_fail_main, NULL, NULL);
}

/* Test: POST body echo over TLS. */

static void _test_post_main(void* arg) {
    (void)arg;
    ASSERT(_cert_gen(HTTPS_CERT, HTTPS_KEY) == 0);

    xylem_http_srv_t* srv = _listen_tls(_echo_handler);
    ASSERT(srv != NULL);
    uint16_t port = _srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "https://127.0.0.1:%u/upload", (unsigned)port);

    char body[4096];
    memset(body, 'Z', sizeof(body));

    xylem_http_tls_t cli_tls = { .skip_verify = true };
    xylem_http_cli_opts_t opts = {0};
    opts.tls = &cli_tls;

    xylem_http_res_t* res = xylem_http_post(
        url, body, sizeof(body), "application/octet-stream", NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == sizeof(body));
    ASSERT(memcmp(xylem_http_res_body(res), body, sizeof(body)) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    remove(HTTPS_CERT);
    remove(HTTPS_KEY);
    xylem_shutdown();
}

static void test_https_post(void) {
    xylem_run(_test_post_main, NULL, NULL);
}

/* Test: connection-pool reuse over TLS. */

static void _test_pool_main(void* arg) {
    (void)arg;
    ASSERT(_cert_gen(HTTPS_CERT, HTTPS_KEY) == 0);

    xylem_http_srv_t* srv = _listen_tls(_path_handler);
    ASSERT(srv != NULL);
    uint16_t port = _srv_port(srv);

    xylem_http_tls_t cli_tls = { .skip_verify = true };
    xylem_http_cli_opts_t opts = {0};
    opts.tls = &cli_tls;

    const char* paths[] = {"/a", "/b", "/c"};
    for (int i = 0; i < 3; i++) {
        char url[64];
        snprintf(url, sizeof(url), "https://127.0.0.1:%u%s",
                 (unsigned)port, paths[i]);
        xylem_http_res_t* res = xylem_http_get(url, NULL, 0, &opts);
        ASSERT(res != NULL);
        ASSERT(xylem_http_res_status(res) == 200);
        ASSERT(xylem_http_res_body_len(res) == 2);
        ASSERT(memcmp(xylem_http_res_body(res), paths[i], 2) == 0);
        xylem_http_res_destroy(res);
    }

    xylem_http_close(srv);
    remove(HTTPS_CERT);
    remove(HTTPS_KEY);
    xylem_shutdown();
}

static void test_https_pool_reuse(void) {
    xylem_run(_test_pool_main, NULL, NULL);
}

/* Main. */

int main(void) {
    test_https_get();
    test_https_pinned_ca();
    test_https_verify_fail();
    test_https_post();
    test_https_pool_reuse();
    printf("All HTTPS tests passed.\n");
    return 0;
}
