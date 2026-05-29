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
#include "assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── URL encode/decode unit tests ─────────────────────────────── */

static void test_url_encode_unreserved(void) {
    size_t len;
    char* enc = xylem_http_url_encode("hello-world_2.0~", 16, &len);
    ASSERT(enc != NULL);
    ASSERT(len == 16);
    ASSERT(strcmp(enc, "hello-world_2.0~") == 0);
    free(enc);
}

static void test_url_encode_reserved(void) {
    size_t len;
    char* enc = xylem_http_url_encode("a b/c", 5, &len);
    ASSERT(enc != NULL);
    ASSERT(strcmp(enc, "a%20b%2Fc") == 0);
    ASSERT(len == 9);
    free(enc);
}

static void test_url_encode_empty(void) {
    size_t len;
    char* enc = xylem_http_url_encode("", 0, &len);
    ASSERT(enc != NULL);
    ASSERT(len == 0);
    ASSERT(strcmp(enc, "") == 0);
    free(enc);
}

static void test_url_decode_basic(void) {
    size_t len;
    char* dec = xylem_http_url_decode("a%20b%2Fc", 9, &len);
    ASSERT(dec != NULL);
    ASSERT(len == 5);
    ASSERT(memcmp(dec, "a b/c", 5) == 0);
    free(dec);
}

static void test_url_decode_passthrough(void) {
    size_t len;
    char* dec = xylem_http_url_decode("hello", 5, &len);
    ASSERT(dec != NULL);
    ASSERT(len == 5);
    ASSERT(strcmp(dec, "hello") == 0);
    free(dec);
}

static void test_url_encode_decode_round_trip(void) {
    const char input[] = "\x01\x7F\x80\xFF hello";
    size_t input_len = sizeof(input) - 1;

    size_t enc_len;
    char* enc = xylem_http_url_encode(input, input_len, &enc_len);
    ASSERT(enc != NULL);

    size_t dec_len;
    char* dec = xylem_http_url_decode(enc, enc_len, &dec_len);
    ASSERT(dec != NULL);
    ASSERT(dec_len == input_len);
    ASSERT(memcmp(dec, input, input_len) == 0);

    free(enc);
    free(dec);
}

/* ─── Response/request NULL accessor tests ─────────────────────── */

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
    ASSERT(xylem_http_req_param(NULL, "id") == NULL);
}

/* ─── Response writer NULL tests ───────────────────────────────── */

static void test_res_set_status_null(void) {
    ASSERT(xylem_http_res_set_status(NULL, 200) == -1);
}

static void test_res_set_header_null(void) {
    ASSERT(xylem_http_res_set_header(NULL, "X-Foo", "bar") == -1);
}

static void test_res_write_null(void) {
    ASSERT(xylem_http_res_write(NULL, "data", 4) == -1);
}

/* ─── CORS tests ───────────────────────────────────────────────── */

static void test_cors_vary_origin(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "http://foo.com";

    xylem_http_hdr_t out[7];
    size_t n = xylem_http_cors_headers(&cors, "http://foo.com",
                                       false, out, 7);
    ASSERT(n >= 2);
    bool found_vary = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(out[i].name, "Vary") == 0) {
            ASSERT(strcmp(out[i].value, "Origin") == 0);
            found_vary = true;
        }
    }
    ASSERT(found_vary);
}

static void test_cors_wildcard_no_vary(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "*";

    xylem_http_hdr_t out[7];
    size_t n = xylem_http_cors_headers(&cors, "http://example.com",
                                       false, out, 7);
    for (size_t i = 0; i < n; i++) {
        ASSERT(strcmp(out[i].name, "Vary") != 0);
    }
}

static void test_cors_wildcard_origin(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "*";

    xylem_http_hdr_t out[7];
    size_t n = xylem_http_cors_headers(&cors, "http://example.com",
                                       false, out, 7);
    ASSERT(n == 1);
    ASSERT(strcmp(out[0].name, "Access-Control-Allow-Origin") == 0);
    ASSERT(strcmp(out[0].value, "*") == 0);
}

static void test_cors_specific_origin(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "http://foo.com, http://bar.com";

    xylem_http_hdr_t out[7];
    size_t n = xylem_http_cors_headers(&cors, "http://bar.com",
                                       false, out, 7);
    ASSERT(n >= 1);
    ASSERT(strcmp(out[0].name, "Access-Control-Allow-Origin") == 0);

    n = xylem_http_cors_headers(&cors, "http://evil.com",
                                false, out, 7);
    ASSERT(n == 0);
}

static void test_cors_credentials_no_wildcard(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins   = "*";
    cors.allow_credentials = true;

    xylem_http_hdr_t out[7];
    size_t n = xylem_http_cors_headers(&cors, "http://example.com",
                                       false, out, 7);
    ASSERT(n >= 2);
    ASSERT(strcmp(out[0].value, "http://example.com") == 0);
    bool found_cred = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(out[i].name, "Access-Control-Allow-Credentials") == 0) {
            ASSERT(strcmp(out[i].value, "true") == 0);
            found_cred = true;
        }
    }
    ASSERT(found_cred);
}

static void test_cors_preflight_headers(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "*";
    cors.allowed_methods = "GET,POST,PUT";
    cors.allowed_headers = "Content-Type,Authorization";
    cors.max_age         = 3600;

    xylem_http_hdr_t out[7];
    size_t n = xylem_http_cors_headers(&cors, "http://example.com",
                                       true, out, 7);
    ASSERT(n == 4);

    bool found_methods = false;
    bool found_headers = false;
    bool found_max_age = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(out[i].name, "Access-Control-Allow-Methods") == 0) {
            found_methods = true;
        }
        if (strcmp(out[i].name, "Access-Control-Allow-Headers") == 0) {
            found_headers = true;
        }
        if (strcmp(out[i].name, "Access-Control-Max-Age") == 0) {
            ASSERT(strcmp(out[i].value, "3600") == 0);
            found_max_age = true;
        }
    }
    ASSERT(found_methods);
    ASSERT(found_headers);
    ASSERT(found_max_age);
}

static void test_cors_null_config(void) {
    xylem_http_hdr_t out[7];
    ASSERT(xylem_http_cors_headers(NULL, "http://example.com",
                                   false, out, 7) == 0);

    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "*";
    ASSERT(xylem_http_cors_headers(&cors, NULL, false, out, 7) == 0);
}

/* ─── Multipart tests ──────────────────────────────────────────── */

static void test_multipart_parse_basic(void) {
    const char* ct = "multipart/form-data; boundary=abc123";
    const char body[] =
        "--abc123\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n"
        "\r\n"
        "value1"
        "\r\n--abc123\r\n"
        "Content-Disposition: form-data; name=\"field2\"\r\n"
        "\r\n"
        "value2"
        "\r\n--abc123--\r\n";

    xylem_http_multipart_t* mp = xylem_http_multipart_parse(
        ct, body, sizeof(body) - 1);
    ASSERT(mp != NULL);
    ASSERT(xylem_http_multipart_count(mp) == 2);

    ASSERT(strcmp(xylem_http_multipart_name(mp, 0), "field1") == 0);
    ASSERT(xylem_http_multipart_data_len(mp, 0) == 6);
    ASSERT(memcmp(xylem_http_multipart_data(mp, 0), "value1", 6) == 0);

    ASSERT(strcmp(xylem_http_multipart_name(mp, 1), "field2") == 0);
    ASSERT(xylem_http_multipart_data_len(mp, 1) == 6);
    ASSERT(memcmp(xylem_http_multipart_data(mp, 1), "value2", 6) == 0);

    ASSERT(xylem_http_multipart_filename(mp, 0) == NULL);
    ASSERT(xylem_http_multipart_content_type(mp, 0) == NULL);

    xylem_http_multipart_destroy(mp);
}

static void test_multipart_with_filename(void) {
    const char* ct = "multipart/form-data; boundary=----WebKit";
    const char body[] =
        "------WebKit\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello world"
        "\r\n------WebKit--\r\n";

    xylem_http_multipart_t* mp = xylem_http_multipart_parse(
        ct, body, sizeof(body) - 1);
    ASSERT(mp != NULL);
    ASSERT(xylem_http_multipart_count(mp) == 1);
    ASSERT(strcmp(xylem_http_multipart_name(mp, 0), "file") == 0);
    ASSERT(strcmp(xylem_http_multipart_filename(mp, 0), "test.txt") == 0);
    ASSERT(strcmp(xylem_http_multipart_content_type(mp, 0), "text/plain") == 0);
    ASSERT(xylem_http_multipart_data_len(mp, 0) == 11);
    ASSERT(memcmp(xylem_http_multipart_data(mp, 0), "hello world", 11) == 0);

    xylem_http_multipart_destroy(mp);
}

static void test_multipart_invalid_boundary(void) {
    ASSERT(xylem_http_multipart_parse("text/plain", "data", 4) == NULL);
    ASSERT(xylem_http_multipart_parse(NULL, "data", 4) == NULL);
    ASSERT(xylem_http_multipart_parse("multipart/form-data; boundary=x",
                                      NULL, 0) == NULL);
}

static void test_multipart_destroy_null(void) {
    xylem_http_multipart_destroy(NULL);
    ASSERT(xylem_http_multipart_count(NULL) == 0);
    ASSERT(xylem_http_multipart_name(NULL, 0) == NULL);
    ASSERT(xylem_http_multipart_data(NULL, 0) == NULL);
    ASSERT(xylem_http_multipart_data_len(NULL, 0) == 0);
}

/* ─── Integration test: coroutine-based server + client ────────── */

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

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    /* Build URL. */
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)port);

    /* Client GET request. */
    xylem_http_res_t* res = xylem_http_get(url, NULL);
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

/* ─── Connection pool test ────────────────────────────────────── */

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

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    /* First request. */
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/a", (unsigned)port);
    xylem_http_res_t* r1 = xylem_http_get(url, NULL);
    ASSERT(r1 != NULL);
    ASSERT(xylem_http_res_status(r1) == 200);
    ASSERT(xylem_http_res_body_len(r1) == 2);
    ASSERT(memcmp(xylem_http_res_body(r1), "/a", 2) == 0);
    xylem_http_res_destroy(r1);

    /* Second request to same host -- should reuse pooled connection. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/b", (unsigned)port);
    xylem_http_res_t* r2 = xylem_http_get(url, NULL);
    ASSERT(r2 != NULL);
    ASSERT(xylem_http_res_status(r2) == 200);
    ASSERT(xylem_http_res_body_len(r2) == 2);
    ASSERT(memcmp(xylem_http_res_body(r2), "/b", 2) == 0);
    xylem_http_res_destroy(r2);

    /* Third request to verify continued reuse. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/c", (unsigned)port);
    xylem_http_res_t* r3 = xylem_http_get(url, NULL);
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

/* ─── Router tests (integration via server+client) ────────────── */

static void _route_hello(xylem_http_res_t* res, xylem_http_req_t* req,
                         void* ud) {
    (void)req; (void)ud;
    xylem_http_res_write(res, "hello", 5);
}

static void _route_param(xylem_http_res_t* res, xylem_http_req_t* req,
                         void* ud) {
    (void)ud;
    const char* id = xylem_http_req_param(req, "id");
    if (id) {
        xylem_http_res_write(res, id, strlen(id));
    } else {
        xylem_http_res_set_status(res, 500);
        xylem_http_res_write(res, "no param", 8);
    }
}

static void _route_wildcard(xylem_http_res_t* res, xylem_http_req_t* req,
                            void* ud) {
    (void)ud;
    const char* url = xylem_http_req_url(req);
    xylem_http_res_write(res, url, strlen(url));
}

static void _route_post_echo(xylem_http_res_t* res, xylem_http_req_t* req,
                             void* ud) {
    (void)ud;
    const void* body = xylem_http_req_body(req);
    size_t body_len = xylem_http_req_body_len(req);
    xylem_http_res_write(res, body, body_len);
}

static int _mw_abort(xylem_http_res_t* res, xylem_http_req_t* req,
                     void* ud) {
    (void)req; (void)ud;
    xylem_http_res_set_status(res, 403);
    xylem_http_res_write(res, "forbidden", 9);
    return -1;
}

static void _router_handler(xylem_http_res_t* res, xylem_http_req_t* req,
                            void* ud) {
    xylem_http_router_t* router = (xylem_http_router_t*)ud;
    xylem_http_router_dispatch(router, res, req);
}

static void _test_router_main(void* arg) {
    (void)arg;

    /* Create router. */
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);

    /* Add routes. */
    ASSERT(xylem_http_router_add(r, "GET", "/api/hello",
                                 _route_hello, NULL) == 0);
    ASSERT(xylem_http_router_add(r, "GET", "/user/:id",
                                 _route_param, NULL) == 0);
    ASSERT(xylem_http_router_add(r, "GET", "/static/*",
                                 _route_wildcard, NULL) == 0);
    ASSERT(xylem_http_router_add(r, "POST", "/echo",
                                 _route_post_echo, NULL) == 0);

    /* Start server. */
    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _router_handler, r, NULL);
    ASSERT(srv != NULL);

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    char url[128];

    /* Test 1: Exact route match. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/api/hello", (unsigned)port);
    xylem_http_res_t* res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 5);
    ASSERT(memcmp(xylem_http_res_body(res), "hello", 5) == 0);
    xylem_http_res_destroy(res);

    /* Test 2: Path parameter capture. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/user/42", (unsigned)port);
    res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 2);
    ASSERT(memcmp(xylem_http_res_body(res), "42", 2) == 0);
    xylem_http_res_destroy(res);

    /* Test 3: Wildcard match. */
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/static/css/app.css", (unsigned)port);
    res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(memcmp(xylem_http_res_body(res), "/static/css/app.css", 19) == 0);
    xylem_http_res_destroy(res);

    /* Test 4: 404 for unmatched route. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/nope", (unsigned)port);
    res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 404);
    ASSERT(xylem_http_res_body_len(res) == 9);
    ASSERT(memcmp(xylem_http_res_body(res), "Not Found", 9) == 0);
    xylem_http_res_destroy(res);

    /* Test 5: Method-specific route (POST /echo). */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/echo", (unsigned)port);
    res = xylem_http_post(url, "data", 4, "text/plain", NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 4);
    ASSERT(memcmp(xylem_http_res_body(res), "data", 4) == 0);
    xylem_http_res_destroy(res);

    /* Test 6: Wrong method -> 404. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/echo", (unsigned)port);
    res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 404);
    ASSERT(xylem_http_res_body_len(res) == 9);
    ASSERT(memcmp(xylem_http_res_body(res), "Not Found", 9) == 0);
    xylem_http_res_destroy(res);

    /* Clean up. */
    xylem_http_close(srv);
    xylem_http_router_destroy(r);
    xylem_shutdown();
}

static void test_router_basic(void) {
    xylem_run(_test_router_main, NULL, NULL);
}

/* Router middleware abort test. */
static void _test_router_mw_abort(void* arg) {
    (void)arg;

    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);

    /* Add middleware that aborts all requests. */
    ASSERT(xylem_http_router_use(r, _mw_abort, NULL) == 0);

    /* Add a route that should never be reached. */
    ASSERT(xylem_http_router_add(r, "GET", "/secret",
                                 _route_hello, NULL) == 0);

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _router_handler, r, NULL);
    ASSERT(srv != NULL);

    uint16_t port = xylem_http_srv_port(srv);
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/secret", (unsigned)port);

    xylem_http_res_t* res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    /* Middleware returned 403 with "forbidden" body. */
    ASSERT(xylem_http_res_status(res) == 403);
    ASSERT(xylem_http_res_body_len(res) == 9);
    ASSERT(memcmp(xylem_http_res_body(res), "forbidden", 9) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_http_router_destroy(r);
    xylem_shutdown();
}

static void test_router_middleware_abort(void) {
    xylem_run(_test_router_mw_abort, NULL, NULL);
}

/* ─── Gzip compression test ──────────────────────────────────── */

static void _test_gzip_handler(xylem_http_res_t* res,
                               xylem_http_req_t* req,
                               void* ud) {
    (void)req; (void)ud;
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    const char* body =
        "hello gzip world, this is a test string that should be "
        "compressed by the server";
    xylem_http_res_write(res, body, strlen(body));
}

static void _test_gzip_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _test_gzip_handler, NULL, NULL);
    ASSERT(srv != NULL);

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    xylem_http_gzip_opts_t gzip = {0};
    gzip.enabled = true;
    gzip.min_size = 1;
    xylem_http_srv_set_gzip(srv, &gzip);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", (unsigned)port);

    /* Request with Accept-Encoding: gzip. */
    xylem_http_hdr_t ae = {"Accept-Encoding", "gzip"};
    xylem_http_opts_t opts = {0};
    opts.headers = &ae;
    opts.header_count = 1;
    xylem_http_res_t* res = xylem_http_get(url, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);

    /* Client auto-decompresses, so body is original plaintext. */
    const char* expected =
        "hello gzip world, this is a test string that should be "
        "compressed by the server";
    ASSERT(xylem_http_res_body_len(res) == strlen(expected));
    ASSERT(memcmp(xylem_http_res_body(res), expected, strlen(expected)) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_gzip_compression(void) {
    xylem_run(_test_gzip_main, NULL, NULL);
}

/* ─── Static file server test ─────────────────────────────────── */

static void _test_static_main(void* arg) {
    (void)arg;

    /* Write a temp file to serve. */
    FILE* f = fopen("_test_static.txt", "w");
    ASSERT(f != NULL);
    fprintf(f, "static content");
    fclose(f);

    /* Create a router with the static file server. */
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);

    xylem_http_static_opts_t opts = {0};
    opts.root = ".";
    opts.max_age = 3600;
    ASSERT(xylem_http_static_serve(r, "/files/", &opts) == 0);

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _router_handler, r, NULL);
    ASSERT(srv != NULL);

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    char url[256];

    /* Test 1: Serve an existing file. */
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/files/_test_static.txt", (unsigned)port);
    xylem_http_res_t* res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 14);
    ASSERT(memcmp(xylem_http_res_body(res), "static content", 14) == 0);

    /* Check Content-Type header. */
    const char* ct = xylem_http_res_header(res, "Content-Type");
    ASSERT(ct != NULL);
    ASSERT(strcmp(ct, "text/plain") == 0);

    /* Check Cache-Control header. */
    const char* cc = xylem_http_res_header(res, "Cache-Control");
    ASSERT(cc != NULL);
    ASSERT(strcmp(cc, "max-age=3600") == 0);

    xylem_http_res_destroy(res);

    /* Test 2: 404 for missing file. */
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/files/nonexistent.txt", (unsigned)port);
    res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 404);
    xylem_http_res_destroy(res);

    /* Test 3: Path traversal rejection (403). */
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/files/sub/../_test_static.txt",
             (unsigned)port);
    res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    /* The ".." segment must be rejected as 403 Forbidden. */
    ASSERT(xylem_http_res_status(res) == 403);
    xylem_http_res_destroy(res);

    /* Test 4: 405 for POST method. */
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/files/_test_static.txt", (unsigned)port);
    res = xylem_http_post(url, "data", 4, "text/plain", NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 405);
    xylem_http_res_destroy(res);

    /* Clean up. */
    xylem_http_close(srv);
    xylem_http_router_destroy(r);
    remove("_test_static.txt");
    xylem_shutdown();
}

static void test_static_serve(void) {
    xylem_run(_test_static_main, NULL, NULL);
}

/* ─── Redirect following test ──────────────────────────────────── */

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

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    char url[128];
    xylem_http_opts_t opts = {0};
    opts.max_redirects = 5;

    /* Test 1: Simple 301 redirect /old -> /new. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/old", (unsigned)port);
    xylem_http_res_t* res = xylem_http_get(url, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 7);
    ASSERT(memcmp(xylem_http_res_body(res), "arrived", 7) == 0);
    xylem_http_res_destroy(res);

    /* Test 2: Redirect chain (302 -> 302 -> 200). */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/chain1", (unsigned)port);
    res = xylem_http_get(url, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 3);
    ASSERT(memcmp(xylem_http_res_body(res), "end", 3) == 0);
    xylem_http_res_destroy(res);

    /* Test 3: Without redirect following, we get the 301 response. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/old", (unsigned)port);
    res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 301);
    xylem_http_res_destroy(res);

    /* Test 4: 303 should change POST to GET. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/see-other", (unsigned)port);
    res = xylem_http_post(url, "body", 4, "text/plain", &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 3);
    ASSERT(memcmp(xylem_http_res_body(res), "GET", 3) == 0);
    xylem_http_res_destroy(res);

    /* Test 5: max_redirects=1 should stop after 1 hop. */
    xylem_http_opts_t opts1 = {0};
    opts1.max_redirects = 1;
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/chain1", (unsigned)port);
    res = xylem_http_get(url, &opts1);
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

/* ─── Cookie jar test ─────────────────────────────────────────── */

static void _test_cookie_handler(xylem_http_res_t* res,
                                 xylem_http_req_t* req,
                                 void* ud) {
    (void)ud;
    const char* cookie = xylem_http_req_header(req, "Cookie");
    if (!cookie) {
        /* First request: set a cookie. */
        xylem_http_res_set_header(res, "Set-Cookie",
                                  "session=abc123; Path=/");
        xylem_http_res_write(res, "set", 3);
    } else {
        /* Second request: echo the cookie. */
        xylem_http_res_write(res, cookie, strlen(cookie));
    }
}

static void _test_cookie_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _test_cookie_handler, NULL, NULL);
    ASSERT(srv != NULL);

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)port);

    xylem_http_cookie_jar_t* jar = xylem_http_cookie_jar_create();
    ASSERT(jar != NULL);

    xylem_http_opts_t opts = {0};
    opts.cookie_jar = jar;

    /* First request: server sets a cookie via Set-Cookie header. */
    xylem_http_res_t* res = xylem_http_get(url, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 3);
    ASSERT(memcmp(xylem_http_res_body(res), "set", 3) == 0);
    xylem_http_res_destroy(res);

    /* Second request: cookie should be sent back automatically. */
    res = xylem_http_get(url, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    /* Body is the echoed Cookie header: "session=abc123" */
    ASSERT(xylem_http_res_body_len(res) == 14);
    ASSERT(memcmp(xylem_http_res_body(res), "session=abc123", 14) == 0);
    xylem_http_res_destroy(res);

    xylem_http_cookie_jar_destroy(jar);
    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_cookie_jar(void) {
    xylem_run(_test_cookie_main, NULL, NULL);
}

/* ─── Basic authentication test ───────────────────────────────── */

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

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/auth", (unsigned)port);

    /* Test 1: No auth -> 401. */
    xylem_http_res_t* res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 401);
    ASSERT(xylem_http_res_body_len(res) == 12);
    ASSERT(memcmp(xylem_http_res_body(res), "unauthorized", 12) == 0);
    xylem_http_res_destroy(res);

    /* Test 2: With auth -> 200, body = base64("user:pass") = "dXNlcjpwYXNz". */
    xylem_http_auth_t auth_cred = {.username = "user", .password = "pass"};
    xylem_http_opts_t opts = {0};
    opts.auth = &auth_cred;

    res = xylem_http_get(url, &opts);
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

/* ─── Expect/100-Continue ─────────────────────────────────────── */

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
    uint16_t port = xylem_http_srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/upload", (unsigned)port);

    char body[4096];
    memset(body, 'A', sizeof(body));

    xylem_http_opts_t opts = {.expect_continue = true};
    xylem_http_res_t* res = xylem_http_post(
        url, body, sizeof(body), "application/octet-stream", &opts);
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

/* ─── Multipart builder unit tests ────────────────────────────── */

static void test_multipart_build_basic(void) {
    xylem_http_multipart_builder_t* b = xylem_http_multipart_build_create();
    ASSERT(b != NULL);

    int rc = xylem_http_multipart_build_field(b, "name", "Alice", 5);
    ASSERT(rc == 0);

    rc = xylem_http_multipart_build_file(
        b, "avatar", "photo.png", "image/png", "\x89PNG", 4);
    ASSERT(rc == 0);

    void* body = NULL;
    size_t body_len = 0;
    char* ct = NULL;
    rc = xylem_http_multipart_build_finish(b, &body, &body_len, &ct);
    ASSERT(rc == 0);
    ASSERT(body != NULL);
    ASSERT(body_len > 0);
    ASSERT(ct != NULL);
    ASSERT(strncmp(ct, "multipart/form-data; boundary=", 30) == 0);

    /* Roundtrip: parse what we built. */
    xylem_http_multipart_t* mp = xylem_http_multipart_parse(ct, body, body_len);
    ASSERT(mp != NULL);
    ASSERT(xylem_http_multipart_count(mp) == 2);

    ASSERT(strcmp(xylem_http_multipart_name(mp, 0), "name") == 0);
    ASSERT(xylem_http_multipart_data_len(mp, 0) == 5);
    ASSERT(memcmp(xylem_http_multipart_data(mp, 0), "Alice", 5) == 0);
    ASSERT(xylem_http_multipart_filename(mp, 0) == NULL);

    ASSERT(strcmp(xylem_http_multipart_name(mp, 1), "avatar") == 0);
    ASSERT(strcmp(xylem_http_multipart_filename(mp, 1), "photo.png") == 0);
    ASSERT(strcmp(xylem_http_multipart_content_type(mp, 1), "image/png") == 0);
    ASSERT(xylem_http_multipart_data_len(mp, 1) == 4);

    xylem_http_multipart_destroy(mp);
    free(body);
    free(ct);
    xylem_http_multipart_build_destroy(b);
}

static void test_multipart_build_empty(void) {
    xylem_http_multipart_builder_t* b = xylem_http_multipart_build_create();
    ASSERT(b != NULL);

    void* body = NULL;
    size_t body_len = 0;
    char* ct = NULL;
    int rc = xylem_http_multipart_build_finish(b, &body, &body_len, &ct);
    ASSERT(rc == -1);

    xylem_http_multipart_build_destroy(b);
}

static void test_multipart_build_destroy_null(void) {
    xylem_http_multipart_build_destroy(NULL);
}

/* ─── Integration: multipart POST ────────────────────────────── */

static void _multipart_echo_handler(xylem_http_res_t* res,
                                    xylem_http_req_t* req,
                                    void* userdata) {
    (void)userdata;
    const char* ct = xylem_http_req_header(req, "Content-Type");
    const void* body = xylem_http_req_body(req);
    size_t body_len = xylem_http_req_body_len(req);

    xylem_http_multipart_t* mp = xylem_http_multipart_parse(ct, body, body_len);
    if (!mp) {
        xylem_http_res_set_status(res, 400);
        xylem_http_res_write(res, "bad multipart", 13);
        return;
    }

    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%zu", xylem_http_multipart_count(mp));
    xylem_http_res_set_status(res, 200);
    xylem_http_res_write(res, buf, (size_t)n);
    xylem_http_multipart_destroy(mp);
}

static void _test_multipart_post_main(void* arg) {
    (void)arg;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _multipart_echo_handler, NULL, NULL);
    ASSERT(srv != NULL);
    uint16_t port = xylem_http_srv_port(srv);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/upload", (unsigned)port);

    xylem_http_multipart_builder_t* b = xylem_http_multipart_build_create();
    ASSERT(b != NULL);
    xylem_http_multipart_build_field(b, "key", "value", 5);
    xylem_http_multipart_build_file(
        b, "file", "data.bin", NULL, "ABCDEF", 6);

    void* body = NULL;
    size_t body_len = 0;
    char* ct = NULL;
    int rc = xylem_http_multipart_build_finish(b, &body, &body_len, &ct);
    ASSERT(rc == 0);

    xylem_http_res_t* res = xylem_http_post(url, body, body_len, ct, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(memcmp(xylem_http_res_body(res), "2", 1) == 0);
    xylem_http_res_destroy(res);

    free(body);
    free(ct);
    xylem_http_multipart_build_destroy(b);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_multipart_post(void) {
    xylem_run(_test_multipart_post_main, NULL, NULL);
}

static void test_sse_build(void) {
    /* Basic event + data. */
    size_t len = 0;
    char* msg = xylem_http_sse_build("update", "hello", &len);
    ASSERT(msg != NULL);
    ASSERT(len > 0);
    ASSERT(strcmp(msg, "event: update\ndata: hello\n\n") == 0);
    ASSERT(len == strlen("event: update\ndata: hello\n\n"));
    free(msg);

    /* Data-only (no event). */
    msg = xylem_http_sse_build(NULL, "world", &len);
    ASSERT(msg != NULL);
    ASSERT(strcmp(msg, "data: world\n\n") == 0);
    ASSERT(len == strlen("data: world\n\n"));
    free(msg);

    /* Multi-line data splits into multiple data: lines. */
    msg = xylem_http_sse_build("msg", "line1\nline2\nline3", &len);
    ASSERT(msg != NULL);
    ASSERT(strcmp(msg, "event: msg\ndata: line1\ndata: line2\ndata: line3\n\n") == 0);
    free(msg);

    /* NULL data returns NULL. */
    ASSERT(xylem_http_sse_build("ev", NULL, NULL) == NULL);

    /* Empty event string is omitted. */
    msg = xylem_http_sse_build("", "test", &len);
    ASSERT(msg != NULL);
    ASSERT(strcmp(msg, "data: test\n\n") == 0);
    free(msg);
}

/* ─── Body limit test ─────────────────────────────────────────── */

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

static void _test_body_limit_main(void* arg) {
    (void)arg;

    xylem_http_srv_opts_t opts = {0};
    opts.max_body_size = 512; /* 512 bytes max */

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _body_limit_handler, NULL, &opts);
    ASSERT(srv != NULL);

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/upload", (unsigned)port);

    /* Body under limit: should get 200. */
    char small_body[256];
    memset(small_body, 'A', sizeof(small_body));
    xylem_http_res_t* res = xylem_http_post(
        url, small_body, sizeof(small_body), "application/octet-stream", NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(memcmp(xylem_http_res_body(res), "256", 3) == 0);
    xylem_http_res_destroy(res);

    /* Body over limit: should get 413. */
    char* big_body = (char*)malloc(1024);
    ASSERT(big_body != NULL);
    memset(big_body, 'B', 1024);
    res = xylem_http_post(
        url, big_body, 1024, "application/octet-stream", NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 413);
    xylem_http_res_destroy(res);
    free(big_body);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_body_limit(void) {
    xylem_run(_test_body_limit_main, NULL, NULL);
}

/* ─── Idle timeout test ───────────────────────────────────────── */

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

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)port);

    /* Normal request should succeed with the timeout configured. */
    xylem_http_res_t* res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 2);
    ASSERT(memcmp(xylem_http_res_body(res), "ok", 2) == 0);
    xylem_http_res_destroy(res);

    /* Second request on reused connection should also succeed. */
    res = xylem_http_get(url, NULL);
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

/* ─── Max requests test ───────────────────────────────────────── */

static void _max_req_handler(xylem_http_res_t* res,
                             xylem_http_req_t* req,
                             void* userdata) {
    (void)req; (void)userdata;
    xylem_http_res_set_status(res, 200);
    xylem_http_res_write(res, "ok", 2);
}

static void _test_max_requests_main(void* arg) {
    (void)arg;

    xylem_http_srv_opts_t opts = {0};
    opts.max_requests = 2;

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 0, _max_req_handler, NULL, &opts);
    ASSERT(srv != NULL);

    uint16_t port = xylem_http_srv_port(srv);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)port);

    /* First 2 requests should succeed on the same connection. */
    xylem_http_res_t* res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    xylem_http_res_destroy(res);

    res = xylem_http_get(url, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    xylem_http_res_destroy(res);

    /*
     * 3rd request: the server closed the connection after 2 requests.
     * The client pool holds the stale conn. The next request will fail
     * on the stale conn (returns NULL) because the client has no
     * transparent retry. This verifies max_requests enforced closure.
     */
    res = xylem_http_get(url, NULL);
    ASSERT(res == NULL);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_max_requests(void) {
    xylem_run(_test_max_requests_main, NULL, NULL);
}

/* ─── Main ─────────────────────────────────────────────────────── */

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

    /* CORS */
    test_cors_vary_origin();
    test_cors_wildcard_no_vary();
    test_cors_wildcard_origin();
    test_cors_specific_origin();
    test_cors_credentials_no_wildcard();
    test_cors_preflight_headers();
    test_cors_null_config();

    /* Multipart */
    test_multipart_parse_basic();
    test_multipart_with_filename();
    test_multipart_invalid_boundary();
    test_multipart_destroy_null();

    /* Integration: coroutine-based server + client */
    test_http_integration();

    /* Connection pool reuse */
    test_pool_reuse();

    /* Router */
    test_router_basic();
    test_router_middleware_abort();

    /* Gzip */
    test_gzip_compression();

    /* Static file server */
    test_static_serve();

    /* Redirect following */
    test_redirect_following();

    /* Cookie jar */
    test_cookie_jar();

    /* Basic authentication */
    test_basic_auth();

    /* Expect/100-Continue */
    test_expect_continue();

    /* Multipart builder */
    test_multipart_build_basic();
    test_multipart_build_empty();
    test_multipart_build_destroy_null();

    /* Integration: multipart POST */
    test_multipart_post();

    /* SSE builder */
    test_sse_build();

    /* Body limit */
    test_body_limit();

    /* Idle timeout */
    test_idle_timeout();

    /* Max requests */
    test_max_requests();

    return 0;
}
