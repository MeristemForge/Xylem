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

    uint16_t port = xylem_http_server_port(srv);
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
    xylem_http_close_server(srv);
    xylem_shutdown();
}

static void test_http_integration(void) {
    xylem_run(_test_http_integration, NULL, NULL);
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

    return 0;
}
