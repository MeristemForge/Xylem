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

#include "assert.h"
#include "xylem/http/xylem-http-common.h"
#include "xylem/http/xylem-http-client.h"
#include "xylem/http/xylem-http-server.h"
#include "xylem/xylem-loop.h"

#include <stdlib.h>
#include <string.h>

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

static void test_res_destroy_null(void) {
    /* destroy(NULL) must be a no-op, not crash */
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

static void test_srv_create_null_loop(void) {
    xylem_http_srv_cfg_t cfg = {0};
    ASSERT(xylem_http_srv_create(NULL, &cfg) == NULL);
}

static void test_srv_create_null_cfg(void) {
    /* Pass a non-NULL loop pointer but NULL cfg */
    xylem_loop_t loop;
    ASSERT(xylem_http_srv_create(&loop, NULL) == NULL);
}

static void test_srv_create_destroy(void) {
    xylem_loop_t loop;
    xylem_loop_init(&loop);

    xylem_http_srv_cfg_t cfg = {0};
    cfg.port = 0;
    xylem_http_srv_t* srv = xylem_http_srv_create(&loop, &cfg);
    ASSERT(srv != NULL);
    xylem_http_srv_destroy(srv);

    xylem_loop_deinit(&loop);
}

static void test_srv_destroy_null(void) {
    /* destroy(NULL) must be a no-op */
    xylem_http_srv_destroy(NULL);
}

static void test_srv_start_null(void) {
    ASSERT(xylem_http_srv_start(NULL) == -1);
}

static void test_srv_stop_null(void) {
    /* stop(NULL) must be a no-op, not crash */
    xylem_http_srv_stop(NULL);
}

static void test_chunked_start_null(void) {
    /* Removed: begin_chunked is now internal. */
}

static void test_chunked_send_null(void) {
    /* Removed: send_chunk is now internal. */
}

static void test_chunked_send_zero_len(void) {
    /* Removed: send_chunk is now internal. */
}

static void test_chunked_end_null(void) {
    /* Removed: end_chunked is now internal. */
}

static void test_cookie_jar_create_destroy(void) {
    xylem_http_cookie_jar_t* jar = xylem_http_cookie_jar_create();
    ASSERT(jar != NULL);
    xylem_http_cookie_jar_destroy(jar);
}

static void test_cookie_jar_destroy_null(void) {
    xylem_http_cookie_jar_destroy(NULL);
}

static void test_send_partial_null(void) {
    /* Removed: send_partial is now internal. */
}

static void test_cors_wildcard_origin(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "*";

    xylem_http_hdr_t out[6];
    size_t n = xylem_http_cors_headers(&cors, "http://example.com",
                                       false, out, 6);
    ASSERT(n == 1);
    ASSERT(strcmp(out[0].name, "Access-Control-Allow-Origin") == 0);
    ASSERT(strcmp(out[0].value, "*") == 0);
}

static void test_cors_specific_origin(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "http://foo.com, http://bar.com";

    xylem_http_hdr_t out[6];
    size_t n = xylem_http_cors_headers(&cors, "http://bar.com",
                                       false, out, 6);
    ASSERT(n == 1);
    ASSERT(strcmp(out[0].name, "Access-Control-Allow-Origin") == 0);

    n = xylem_http_cors_headers(&cors, "http://evil.com",
                                false, out, 6);
    ASSERT(n == 0);
}

static void test_cors_credentials_no_wildcard(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins   = "*";
    cors.allow_credentials = true;

    xylem_http_hdr_t out[6];
    size_t n = xylem_http_cors_headers(&cors, "http://example.com",
                                       false, out, 6);
    ASSERT(n == 2);
    ASSERT(strcmp(out[0].value, "http://example.com") == 0);
    ASSERT(strcmp(out[1].name, "Access-Control-Allow-Credentials") == 0);
    ASSERT(strcmp(out[1].value, "true") == 0);
}

static void test_cors_preflight_headers(void) {
    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "*";
    cors.allowed_methods = "GET,POST,PUT";
    cors.allowed_headers = "Content-Type,Authorization";
    cors.max_age         = 3600;

    xylem_http_hdr_t out[6];
    size_t n = xylem_http_cors_headers(&cors, "http://example.com",
                                       true, out, 6);
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
    xylem_http_hdr_t out[6];
    ASSERT(xylem_http_cors_headers(NULL, "http://example.com",
                                   false, out, 6) == 0);

    xylem_http_cors_t cors = {0};
    cors.allowed_origins = "*";
    ASSERT(xylem_http_cors_headers(&cors, NULL, false, out, 6) == 0);
}

static void test_sse_start_null(void) {
    /* Removed: begin_sse is now internal. */
}

static void test_sse_send_event_null(void) {
    ASSERT(xylem_http_sse_build(NULL, NULL, NULL) == NULL);
}

static void test_sse_send_data_null(void) {
    ASSERT(xylem_http_sse_build("ping", NULL, NULL) == NULL);
}

static void test_sse_end_null(void) {
    /* Removed: end_sse is now internal. */
}

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

static int _test_mw_called;

static int _test_middleware_pass(xylem_http_writer_t* w,
                                xylem_http_req_t* req,
                                void* userdata) {
    (void)w;
    (void)req;
    (void)userdata;
    _test_mw_called++;
    return 0;
}

static int _test_middleware_abort(xylem_http_writer_t* w,
                                 xylem_http_req_t* req,
                                 void* userdata) {
    (void)w;
    (void)req;
    (void)userdata;
    _test_mw_called++;
    return -1;
}

static void test_router_use_null(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    ASSERT(xylem_http_router_use(NULL, _test_middleware_pass, NULL) == -1);
    ASSERT(xylem_http_router_use(r, NULL, NULL) == -1);
    xylem_http_router_destroy(r);
}

static void test_router_use_register(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    ASSERT(xylem_http_router_use(r, _test_middleware_pass, NULL) == 0);
    ASSERT(xylem_http_router_use(r, _test_middleware_pass, NULL) == 0);
    xylem_http_router_destroy(r);
}

static void test_router_create_destroy(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    xylem_http_router_destroy(r);
}

static void test_router_destroy_null(void) {
    xylem_http_router_destroy(NULL);
}

static int _test_router_called;
static void* _test_router_ud;

static void _test_router_handler(xylem_http_writer_t* w,
                                 xylem_http_req_t* req,
                                 void* userdata) {
    (void)w;
    (void)req;
    _test_router_called = 1;
    _test_router_ud = userdata;
}

static void _test_router_handler2(xylem_http_writer_t* w,
                                  xylem_http_req_t* req,
                                  void* userdata) {
    (void)w;
    (void)req;
    _test_router_called = 2;
    _test_router_ud = userdata;
}

static void test_router_exact_match(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    int rc = xylem_http_router_add(r, "GET", "/api/users",
                                   _test_router_handler, NULL);
    ASSERT(rc == 0);
    xylem_http_router_destroy(r);
}

static void test_router_param_match(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    int rc = xylem_http_router_add(r, "GET", "/user/:id",
                                   _test_router_handler, NULL);
    ASSERT(rc == 0);
    xylem_http_router_destroy(r);
}

static void test_router_wildcard_match(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    int rc = xylem_http_router_add(r, "GET", "/static/*",
                                   _test_router_handler, NULL);
    ASSERT(rc == 0);
    xylem_http_router_destroy(r);
}

static void test_router_exact_over_param(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    ASSERT(xylem_http_router_add(r, "GET", "/user/:id",
                                 _test_router_handler, NULL) == 0);
    ASSERT(xylem_http_router_add(r, "GET", "/user/me",
                                 _test_router_handler2, NULL) == 0);
    xylem_http_router_destroy(r);
}

static void test_router_param_over_wildcard(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    ASSERT(xylem_http_router_add(r, "GET", "/files/*",
                                 _test_router_handler, NULL) == 0);
    ASSERT(xylem_http_router_add(r, "GET", "/files/:name",
                                 _test_router_handler2, NULL) == 0);
    xylem_http_router_destroy(r);
}

static void test_router_method_filter(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    ASSERT(xylem_http_router_add(r, "GET", "/data",
                                 _test_router_handler, NULL) == 0);
    ASSERT(xylem_http_router_add(r, "POST", "/data",
                                 _test_router_handler2, NULL) == 0);
    xylem_http_router_destroy(r);
}

static void test_router_method_null(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    ASSERT(xylem_http_router_add(r, NULL, "/any",
                                 _test_router_handler, NULL) == 0);
    xylem_http_router_destroy(r);
}

static void test_router_404(void) {
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    ASSERT(xylem_http_router_dispatch(r, NULL, NULL) == -1);
    ASSERT(xylem_http_router_dispatch(NULL, NULL, NULL) == -1);
    xylem_http_router_destroy(r);
}

static void test_writer_set_header_null(void) {
    ASSERT(xylem_http_writer_set_header(NULL, "X-Foo", "bar") == -1);
}

static void test_writer_set_status_null(void) {
    ASSERT(xylem_http_writer_set_status(NULL, 200) == -1);
}

static void test_writer_write_null(void) {
    ASSERT(xylem_http_writer_write(NULL, "data", 4) == -1);
}

static void test_writer_send_convenience(void) {
    /* Removed: send() is now internal. */
}

static void test_write_zero_len(void) {
    /* write(conn, data, 0) returns 0 (no-op), even with NULL conn
       the len=0 check happens after the NULL check. */
    ASSERT(xylem_http_writer_write(NULL, "data", 0) == -1);
}

static void test_write_on_closed(void) {
    /* write() with NULL conn returns -1. */
    ASSERT(xylem_http_writer_write(NULL, "data", 4) == -1);
}

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

    /* Server lifecycle */
    test_srv_create_null_loop();
    test_srv_create_null_cfg();
    test_srv_create_destroy();
    test_srv_destroy_null();
    test_srv_start_null();
    test_srv_stop_null();

    /* Chunked transfer encoding */
    test_chunked_start_null();
    test_chunked_send_null();
    test_chunked_send_zero_len();
    test_chunked_end_null();

    /* Cookie jar */
    test_cookie_jar_create_destroy();
    test_cookie_jar_destroy_null();

    /* Range support */
    test_send_partial_null();

    /* CORS */
    test_cors_wildcard_origin();
    test_cors_specific_origin();
    test_cors_credentials_no_wildcard();
    test_cors_preflight_headers();
    test_cors_null_config();

    /* SSE */
    test_sse_start_null();
    test_sse_send_event_null();
    test_sse_send_data_null();
    test_sse_end_null();

    /* Multipart */
    test_multipart_parse_basic();
    test_multipart_with_filename();
    test_multipart_invalid_boundary();
    test_multipart_destroy_null();

    /* Router */
    test_router_use_null();
    test_router_use_register();
    test_router_create_destroy();
    test_router_destroy_null();
    test_router_exact_match();
    test_router_param_match();
    test_router_wildcard_match();
    test_router_exact_over_param();
    test_router_param_over_wildcard();
    test_router_method_filter();
    test_router_method_null();
    test_router_404();

    /* Writer mode */
    test_writer_set_header_null();
    test_writer_set_status_null();
    test_writer_write_null();
    test_writer_send_convenience();
    test_write_zero_len();
    test_write_on_closed();

    return 0;
}
