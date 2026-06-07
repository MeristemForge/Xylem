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

#include "xylem.h"
#include "xylem/encoding/xylem-url.h"
#include "assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t _srv_port(xylem_http_srv_t* srv) {
    uint16_t port = 0;
    xylem_http_srv_addr(srv, NULL, 0, &port);
    return port;
}

/* URL encode/decode unit tests. */

static void test_url_encode_unreserved(void) {
    uint8_t buf[64];
    int len = xylem_url_encode((const uint8_t*)"hello-world_2.0~", 16, buf, 64);
    ASSERT(len == 16);
    ASSERT(memcmp(buf, "hello-world_2.0~", 16) == 0);
}

static void test_url_encode_reserved(void) {
    uint8_t buf[64];
    int len = xylem_url_encode((const uint8_t*)"a b/c", 5, buf, 64);
    ASSERT(len == 9);
    ASSERT(memcmp(buf, "a%20b%2Fc", 9) == 0);
}

static void test_url_encode_empty(void) {
    uint8_t buf[1];
    int len = xylem_url_encode((const uint8_t*)"", 0, buf, 1);
    ASSERT(len == 0);
}

static void test_url_decode_basic(void) {
    uint8_t buf[64];
    int len = xylem_url_decode((const uint8_t*)"a%20b%2Fc", 9, buf, 64);
    ASSERT(len == 5);
    ASSERT(memcmp(buf, "a b/c", 5) == 0);
}

static void test_url_decode_passthrough(void) {
    uint8_t buf[64];
    int len = xylem_url_decode((const uint8_t*)"hello", 5, buf, 64);
    ASSERT(len == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);
}

static void test_url_encode_decode_round_trip(void) {
    const uint8_t input[] = {0x01, 0x7F, 0x80, 0xFF, ' ', 'h', 'e', 'l', 'l', 'o'};
    int input_len = 10;

    uint8_t enc[64];
    int enc_len = xylem_url_encode(input, input_len, enc, 64);
    ASSERT(enc_len > 0);

    uint8_t dec[64];
    int dec_len = xylem_url_decode(enc, enc_len, dec, 64);
    ASSERT(dec_len == input_len);
    ASSERT(memcmp(dec, input, (size_t)input_len) == 0);
}

/* Response/request NULL accessor tests. */

static void test_res_destroy_null(void) {
    xylem_http_res_destroy(NULL);
}

static void test_res_accessors_null(void) {
    ASSERT(xylem_http_res_status(NULL) == 0);
    ASSERT(xylem_http_res_header(NULL, "Host") == NULL);
    ASSERT(xylem_http_res_body(NULL) == NULL);
    ASSERT(xylem_http_res_body_len(NULL) == 0);
}

static void test_req_accessors_null(void) {
    ASSERT(xylem_http_req_method(NULL) == NULL);
    ASSERT(xylem_http_req_url(NULL) == NULL);
    ASSERT(xylem_http_req_header(NULL, "Host") == NULL);
    ASSERT(xylem_http_req_body(NULL) == NULL);
    ASSERT(xylem_http_req_body_len(NULL) == 0);
}

/* Response writer NULL tests. */

static void test_res_set_status_null(void) {
    ASSERT(xylem_http_res_set_status(NULL, 200) == -1);
}

static void test_res_set_header_null(void) {
    ASSERT(xylem_http_res_set_header(NULL, "X-Foo", "bar") == -1);
}

static void test_res_write_null(void) {
    ASSERT(xylem_http_res_write(NULL, "data", 4) == -1);
}



/* Integration test: coroutine-based server + client. */

static void _hello_handler(xylem_http_res_t* res,
                           xylem_http_req_t* req,
                           void* userdata) {
    (void)req;
    (void)userdata;
    xylem_http_res_set_status(res, 200);
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    xylem_http_res_write(res, "hello", 5);
}

static void _test_http_integration(void* arg) {
    (void)arg;

    /* Start server on a random port. */
    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _hello_handler, NULL, NULL);
    ASSERT(srv != NULL);

    uint16_t port = _srv_port(srv);
    ASSERT(port != 0);

    /* Build URL. */
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)port);

    /* Client GET request. */
    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 5);
    ASSERT(memcmp(xylem_http_res_body(res), "hello", 5) == 0);

    const char* ct = xylem_http_res_header(res, "Content-Type");
    ASSERT(ct != NULL);
    ASSERT(strcmp(ct, "text/plain") == 0);

    xylem_http_res_destroy(res);

    /* Clean up. */
    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_http_integration(void) {
    xylem_run(_test_http_integration, NULL, NULL);
}

/* Connection pool test. */

static void _pool_handler(xylem_http_res_t* res,
                          xylem_http_req_t* req,
                          void* userdata) {
    (void)userdata;
    const char* url = xylem_http_req_url(req);
    xylem_http_res_set_status(res, 200);
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    xylem_http_res_write(res, url, strlen(url));
}

static void _test_pool_main(void* arg) {
    (void)arg;

    /* Start server on a random port. */
    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _pool_handler, NULL, NULL);
    ASSERT(srv != NULL);

    uint16_t port = _srv_port(srv);
    ASSERT(port != 0);

    /* First request. */
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/a", (unsigned)port);
    xylem_http_res_t* r1 = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(r1 != NULL);
    ASSERT(xylem_http_res_status(r1) == 200);
    ASSERT(xylem_http_res_body_len(r1) == 2);
    ASSERT(memcmp(xylem_http_res_body(r1), "/a", 2) == 0);
    xylem_http_res_destroy(r1);

    /* Second request to same host -- should reuse pooled connection. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/b", (unsigned)port);
    xylem_http_res_t* r2 = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(r2 != NULL);
    ASSERT(xylem_http_res_status(r2) == 200);
    ASSERT(xylem_http_res_body_len(r2) == 2);
    ASSERT(memcmp(xylem_http_res_body(r2), "/b", 2) == 0);
    xylem_http_res_destroy(r2);

    /* Third request to verify continued reuse. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/c", (unsigned)port);
    xylem_http_res_t* r3 = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(r3 != NULL);
    ASSERT(xylem_http_res_status(r3) == 200);
    ASSERT(xylem_http_res_body_len(r3) == 2);
    ASSERT(memcmp(xylem_http_res_body(r3), "/c", 2) == 0);
    xylem_http_res_destroy(r3);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_pool_reuse(void) {
    xylem_run(_test_pool_main, NULL, NULL);
}




/* Redirect following test. */

static void _test_redirect_handler(xylem_http_res_t* res,
                                   xylem_http_req_t* req,
                                   void* ud) {
    (void)ud;
    const char* url = xylem_http_req_url(req);

    if (strcmp(url, "/old") == 0) {
        xylem_http_res_set_status(res, 301);
        xylem_http_res_set_header(res, "Location", "/new");
        xylem_http_res_write(res, "", 0);
    } else if (strcmp(url, "/new") == 0) {
        xylem_http_res_write(res, "arrived", 7);
    } else if (strcmp(url, "/chain1") == 0) {
        xylem_http_res_set_status(res, 302);
        xylem_http_res_set_header(res, "Location", "/chain2");
        xylem_http_res_write(res, "", 0);
    } else if (strcmp(url, "/chain2") == 0) {
        xylem_http_res_set_status(res, 302);
        xylem_http_res_set_header(res, "Location", "/chain3");
        xylem_http_res_write(res, "", 0);
    } else if (strcmp(url, "/chain3") == 0) {
        xylem_http_res_write(res, "end", 3);
    } else if (strcmp(url, "/see-other") == 0) {
        /* 303 should change method to GET. */
        xylem_http_res_set_status(res, 303);
        xylem_http_res_set_header(res, "Location", "/get-only");
        xylem_http_res_write(res, "", 0);
    } else if (strcmp(url, "/get-only") == 0) {
        /* Echo the method to verify it changed to GET. */
        const char* method = xylem_http_req_method(req);
        xylem_http_res_write(res, method, strlen(method));
    } else {
        xylem_http_res_set_status(res, 404);
        xylem_http_res_write(res, "not found", 9);
    }
}

static void _test_redirect_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _test_redirect_handler, NULL, NULL);
    ASSERT(srv != NULL);

    uint16_t port = _srv_port(srv);
    ASSERT(port != 0);

    char url[128];
    xylem_http_cli_opts_t opts = {0};
    opts.max_redirects = 5;

    /* Test 1: Simple 301 redirect /old -> /new. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/old", (unsigned)port);
    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 7);
    ASSERT(memcmp(xylem_http_res_body(res), "arrived", 7) == 0);
    xylem_http_res_destroy(res);

    /* Test 2: Redirect chain (302 -> 302 -> 200). */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/chain1", (unsigned)port);
    res = xylem_http_get(url, NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 3);
    ASSERT(memcmp(xylem_http_res_body(res), "end", 3) == 0);
    xylem_http_res_destroy(res);

    /**
     * Test 3: With redirects disabled (max_redirects = 0), we get the
     * 301 response directly. NULL opts would mean "default 10 hops".
     */
    xylem_http_cli_opts_t opts_noredir = {0};
    opts_noredir.max_redirects = 0;
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/old", (unsigned)port);
    res = xylem_http_get(url, NULL, 0, &opts_noredir);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 301);
    xylem_http_res_destroy(res);

    /* Test 4: 303 should change POST to GET. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/see-other", (unsigned)port);
    res = xylem_http_post(url, "body", 4, "text/plain", NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 3);
    ASSERT(memcmp(xylem_http_res_body(res), "GET", 3) == 0);
    xylem_http_res_destroy(res);

    /* Test 5: max_redirects=1 should stop after 1 hop. */
    xylem_http_cli_opts_t opts1 = {0};
    opts1.max_redirects = 1;
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/chain1", (unsigned)port);
    res = xylem_http_get(url, NULL, 0, &opts1);
    ASSERT(res != NULL);
    /* After 1 redirect: chain1->chain2, chain2 responds 302 */
    ASSERT(xylem_http_res_status(res) == 302);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_redirect_following(void) {
    xylem_run(_test_redirect_main, NULL, NULL);
}


/* Basic authentication test. */

static void _auth_handler(xylem_http_res_t* res,
                          xylem_http_req_t* req,
                          void* ud) {
    (void)ud;
    const char* auth = xylem_http_req_header(req, "Authorization");
    if (!auth) {
        xylem_http_res_set_status(res, 401);
        xylem_http_res_write(res, "unauthorized", 12);
        return;
    }

    /* Expect "Basic <base64>" */
    if (strncmp(auth, "Basic ", 6) != 0) {
        xylem_http_res_set_status(res, 401);
        xylem_http_res_write(res, "bad scheme", 10);
        return;
    }

    /* Echo back the base64 portion. */
    const char* b64 = auth + 6;
    xylem_http_res_set_status(res, 200);
    xylem_http_res_write(res, b64, strlen(b64));
}

static void _test_basic_auth_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _auth_handler, NULL, NULL);
    ASSERT(srv != NULL);

    uint16_t port = _srv_port(srv);
    ASSERT(port != 0);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/auth", (unsigned)port);

    /* Test 1: No auth -> 401. */
    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 401);
    ASSERT(xylem_http_res_body_len(res) == 12);
    ASSERT(memcmp(xylem_http_res_body(res), "unauthorized", 12) == 0);
    xylem_http_res_destroy(res);

    /* Test 2: With auth -> 200, body = base64("user:pass") = "dXNlcjpwYXNz". */
    int   auth_size = xylem_http_basic_auth_size(4, 4);
    char* auth_val  = (char*)malloc((size_t)auth_size);
    ASSERT(auth_val != NULL);
    ASSERT(xylem_http_basic_auth("user", "pass", auth_val, auth_size) > 0);
    xylem_http_hdr_t auth_hdr = {"Authorization", auth_val};

    res = xylem_http_get(url, &auth_hdr, 1, NULL);
    free(auth_val);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 12);
    ASSERT(memcmp(xylem_http_res_body(res), "dXNlcjpwYXNz", 12) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_basic_auth(void) {
    xylem_run(_test_basic_auth_main, NULL, NULL);
}

/* Expect/100-Continue. */

static void _expect_handler(xylem_http_res_t* res,
                            xylem_http_req_t* req,
                            void* userdata) {
    (void)userdata;
    size_t blen = xylem_http_req_body_len(req);
    xylem_http_res_set_status(res, 200);
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%zu", blen);
    xylem_http_res_write(res, buf, (size_t)n);
}

static void _test_expect_continue_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _expect_handler, NULL, NULL);
    ASSERT(srv != NULL);
    uint16_t port = _srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/upload", (unsigned)port);

    char body[4096];
    memset(body, 'A', sizeof(body));

    xylem_http_res_t* res = xylem_http_post(
        url, body, sizeof(body), "application/octet-stream", NULL, 0, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(memcmp(xylem_http_res_body(res), "4096", 4) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_expect_continue(void) {
    xylem_run(_test_expect_continue_main, NULL, NULL);
}

/* 100-Continue server reply test. */

static void _test_100_server_reply_main(void* arg) {
    (void)arg;
    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _expect_handler, NULL, NULL);
    ASSERT(srv != NULL);
    uint16_t port = _srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/upload", (unsigned)port);

    char body[1024];
    memset(body, 'X', sizeof(body));

    uint64_t start = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    xylem_http_res_t* res = xylem_http_post(
        url, body, sizeof(body), "application/octet-stream", NULL, 0, NULL);
    uint64_t elapsed = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) - start;

    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    /* Should complete quickly since server sends 100 immediately. */
    ASSERT(elapsed < 500);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_100_server_reply(void) {
    xylem_run(_test_100_server_reply_main, NULL, NULL);
}

/* Content-Length response mode test. */

static void _test_content_length_main(void* arg) {
    (void)arg;
    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _hello_handler, NULL, NULL);
    ASSERT(srv != NULL);
    uint16_t port = _srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", (unsigned)port);

    xylem_http_res_t* r = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(r != NULL);
    ASSERT(xylem_http_res_status(r) == 200);
    /* Single-write response should use Content-Length, not chunked. */
    const char* cl = xylem_http_res_header(r, "Content-Length");
    ASSERT(cl != NULL);
    ASSERT(strcmp(cl, "5") == 0);
    ASSERT(xylem_http_res_body_len(r) == 5);
    ASSERT(memcmp(xylem_http_res_body(r), "hello", 5) == 0);
    xylem_http_res_destroy(r);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_content_length_mode(void) {
    xylem_run(_test_content_length_main, NULL, NULL);
}



/* Body limit test. */

static void _body_limit_handler(xylem_http_res_t* res,
                                xylem_http_req_t* req,
                                void* userdata) {
    (void)userdata;
    size_t blen = xylem_http_req_body_len(req);
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%zu", blen);
    xylem_http_res_set_status(res, 200);
    xylem_http_res_write(res, buf, (size_t)n);
}


/* Idle timeout test. */

static void _idle_timeout_handler(xylem_http_res_t* res,
                                  xylem_http_req_t* req,
                                  void* userdata) {
    (void)req; (void)userdata;
    xylem_http_res_set_status(res, 200);
    xylem_http_res_write(res, "ok", 2);
}

static void _test_idle_timeout_main(void* arg) {
    (void)arg;

    xylem_http_srv_opts_t opts = {0};
    opts.idle_timeout_ms = 5000; /* 5 seconds */

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _idle_timeout_handler, NULL, &opts);
    ASSERT(srv != NULL);

    uint16_t port = _srv_port(srv);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)port);

    /* Normal request should succeed with the timeout configured. */
    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 2);
    ASSERT(memcmp(xylem_http_res_body(res), "ok", 2) == 0);
    xylem_http_res_destroy(res);

    /* Second request on reused connection should also succeed. */
    res = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 2);
    ASSERT(memcmp(xylem_http_res_body(res), "ok", 2) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_idle_timeout(void) {
    xylem_run(_test_idle_timeout_main, NULL, NULL);
}




/* Plain-HTTP proxy (absolute-form forwarding). */

/**
 * Acts as the proxy: a plain-HTTP proxy receives the request line in
 * absolute-form (full URL), so the server echoes back the URL it parsed.
 * The client points at this server via opts.proxy; if the wiring is
 * correct the echoed URL is the absolute target, not just the path.
 */
static void _proxy_handler(xylem_http_res_t* res,
                           xylem_http_req_t* req,
                           void* userdata) {
    (void)userdata;
    const char* url = xylem_http_req_url(req);
    xylem_http_res_set_status(res, 200);
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    xylem_http_res_write(res, url, strlen(url));
}

static void _test_proxy_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _proxy_handler, NULL, NULL);
    ASSERT(srv != NULL);

    uint16_t port = _srv_port(srv);
    ASSERT(port != 0);

    /* Route the request through the just-started server as a proxy. */
    xylem_http_proxy_t proxy = {0};
    proxy.host = "127.0.0.1";
    proxy.port = port;

    xylem_http_cli_opts_t opts = {0};
    opts.proxy = &proxy;

    /**
     * Target a different (unconnectable) host: the request must reach the
     * proxy, and the proxy must see the absolute-form URL.
     */
    const char* target = "http://target.example/path/page";
    xylem_http_res_t* res = xylem_http_get(target, NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);

    /* The proxy echoes the request-line URL: it must be absolute-form. */
    size_t blen = xylem_http_res_body_len(res);
    ASSERT(blen == strlen(target));
    ASSERT(memcmp(xylem_http_res_body(res), target, blen) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_proxy_plain(void) {
    xylem_run(_test_proxy_main, NULL, NULL);
}


/* Main. */

int main(void) {
    /* URL percent-encoding */
    test_url_encode_unreserved();
    test_url_encode_reserved();
    test_url_encode_empty();
    test_url_decode_basic();
    test_url_decode_passthrough();
    test_url_encode_decode_round_trip();

    /* Response accessors */
    test_res_destroy_null();
    test_res_accessors_null();

    /* Request accessors */
    test_req_accessors_null();

    /* Response writer */
    test_res_set_status_null();
    test_res_set_header_null();
    test_res_write_null();


    /* Integration: coroutine-based server + client */
    test_http_integration();

    /* Connection pool reuse */
    test_pool_reuse();

    /* Redirect following */
    test_redirect_following();

    /* Basic authentication */
    test_basic_auth();

    /* Expect/100-Continue */
    test_expect_continue();

    /* 100-Continue server reply (fast path) */
    test_100_server_reply();

    /* Content-Length response mode */
    test_content_length_mode();

    /* Idle timeout */
    test_idle_timeout();

    /* Plain-HTTP proxy (absolute-form forwarding) */
    test_proxy_plain();


    return 0;
}
