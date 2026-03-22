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
#include "http-common.h"
#include "xylem/http/xylem-http-utils.h"
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
    xylem_http_cli_res_destroy(NULL);
}

static void test_res_accessors_null(void) {
    ASSERT(xylem_http_cli_res_status(NULL) == 0);
    ASSERT(xylem_http_cli_res_header(NULL, "Host") == NULL);
    ASSERT(xylem_http_cli_res_body(NULL) == NULL);
    ASSERT(xylem_http_cli_res_body_len(NULL) == 0);
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

static void test_req_serialize_custom_headers(void) {
    http_url_t url;
    http_url_parse("http://example.com/path", &url);

    xylem_http_hdr_t hdrs[] = {
        { "Authorization", "Bearer token123" },
        { "X-Custom", "value" },
    };

    size_t len;
    char* buf = http_req_serialize("GET", &url, NULL, 0, NULL, false, &len,
                                   hdrs, 2);
    ASSERT(buf != NULL);
    ASSERT(strstr(buf, "Authorization: Bearer token123\r\n") != NULL);
    ASSERT(strstr(buf, "X-Custom: value\r\n") != NULL);
    ASSERT(strstr(buf, "Host: example.com\r\n") != NULL);
    free(buf);
}

static void test_req_serialize_override_host(void) {
    http_url_t url;
    http_url_parse("http://example.com/path", &url);

    xylem_http_hdr_t hdrs[] = {
        { "Host", "custom-host.com" },
    };

    size_t len;
    char* buf = http_req_serialize("GET", &url, NULL, 0, NULL, false, &len,
                                   hdrs, 1);
    ASSERT(buf != NULL);
    /* Custom Host present */
    ASSERT(strstr(buf, "Host: custom-host.com\r\n") != NULL);
    /* Auto-generated Host absent */
    ASSERT(strstr(buf, "Host: example.com\r\n") == NULL);
    free(buf);
}

static void test_req_serialize_override_content_type(void) {
    http_url_t url;
    http_url_parse("http://example.com/", &url);

    xylem_http_hdr_t hdrs[] = {
        { "content-type", "text/plain" },
    };

    size_t len;
    char* buf = http_req_serialize("POST", &url, "body", 4,
                                   "application/json", false, &len,
                                   hdrs, 1);
    ASSERT(buf != NULL);
    /* Custom Content-Type present */
    ASSERT(strstr(buf, "content-type: text/plain\r\n") != NULL);
    /* Auto-generated Content-Type absent */
    ASSERT(strstr(buf, "Content-Type: application/json\r\n") == NULL);
    free(buf);
}

static void test_req_serialize_no_custom_headers(void) {
    http_url_t url;
    http_url_parse("http://example.com/", &url);

    size_t len_with;
    char* buf_with = http_req_serialize("GET", &url, NULL, 0, NULL, false,
                                        &len_with, NULL, 0);
    ASSERT(buf_with != NULL);
    ASSERT(strstr(buf_with, "Host: example.com\r\n") != NULL);
    ASSERT(strstr(buf_with, "Connection: keep-alive\r\n") != NULL);
    free(buf_with);
}

static void test_req_serialize_custom_headers_before_auto(void) {
    http_url_t url;
    http_url_parse("http://example.com/", &url);

    xylem_http_hdr_t hdrs[] = {
        { "X-First", "1" },
    };

    size_t len;
    char* buf = http_req_serialize("GET", &url, NULL, 0, NULL, false, &len,
                                   hdrs, 1);
    ASSERT(buf != NULL);
    /* Custom header appears before Host */
    char* custom_pos = strstr(buf, "X-First: 1\r\n");
    char* host_pos   = strstr(buf, "Host: example.com\r\n");
    ASSERT(custom_pos != NULL);
    ASSERT(host_pos != NULL);
    ASSERT(custom_pos < host_pos);
    free(buf);
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

    /* Custom headers */
    test_req_serialize_custom_headers();
    test_req_serialize_override_host();
    test_req_serialize_override_content_type();
    test_req_serialize_no_custom_headers();
    test_req_serialize_custom_headers_before_auto();

    return 0;
}
