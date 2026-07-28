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

#include "net/http/http-utils.h"
#include "assert.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*_handler_t)(xylem_http_writer_t*, xylem_http_req_t*, void*);
typedef void (*_body_t)(uint16_t port);

typedef struct {
    char*  buf;
    size_t len;
    size_t cap;
    int    max_write_len;
    bool   oversized_write;
    bool   closed;
} _upgrade_transport_t;

static uint16_t _srv_port(xylem_http_srv_t* srv) {
    uint16_t port = 0;
    xylem_http_srv_addr(srv, NULL, 0, &port);
    return port;
}

static int _upgrade_read(void* conn, void* buf, int len) {
    (void)conn;
    (void)buf;
    (void)len;
    return -1;
}

static int _upgrade_write(void* conn, const void* data, int len) {
    _upgrade_transport_t* t = (_upgrade_transport_t*)conn;

    if (len < 0) {
        return -1;
    }
    if (t->max_write_len > 0 && len > t->max_write_len) {
        t->oversized_write = true;
        return -1;
    }
    if (t->len + (size_t)len + 1 > t->cap) {
        return -1;
    }

    memcpy(t->buf + t->len, data, (size_t)len);
    t->len += (size_t)len;
    t->buf[t->len] = '\0';
    return 0;
}

static void _upgrade_close(void* conn) {
    _upgrade_transport_t* t = (_upgrade_transport_t*)conn;
    t->closed = true;
}

typedef struct {
    _handler_t             handler;
    xylem_http_srv_opts_t* opts;
    _body_t                body;
} _plan_t;

static void _serve_main(void* arg) {
    _plan_t* p = (_plan_t*)arg;

    xylem_http_srv_opts_t opts =
        p->opts ? *p->opts : (xylem_http_srv_opts_t){0};
    opts.idle_timeout_ms = 100;

    xylem_http_srv_t* srv =
        xylem_http_listen("127.0.0.1", 0, p->handler, NULL, &opts);
    ASSERT(srv != NULL);

    uint16_t port = _srv_port(srv);
    ASSERT(port != 0);

    p->body(port);

    xylem_http_shutdown(srv, 5000);
}

static void _serve(_handler_t handler, xylem_http_srv_opts_t* opts,
                   _body_t body) {
    _plan_t plan = {handler, opts, body};
    _serve_main(&plan);
}

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
    const uint8_t input[] = {0x01, 0x7F, 0x80, 0xFF, ' ',
                             'h',  'e',  'l',  'l',  'o'};
    int input_len = 10;

    uint8_t enc[64];
    int     enc_len = xylem_url_encode(input, input_len, enc, 64);
    ASSERT(enc_len > 0);

    uint8_t dec[64];
    int     dec_len = xylem_url_decode(enc, enc_len, dec, 64);
    ASSERT(dec_len == input_len);
    ASSERT(memcmp(dec, input, (size_t)input_len) == 0);
}

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

static void test_writer_set_status_null(void) {
    ASSERT(xylem_http_writer_set_status(NULL, 200) == -1);
}

static void test_writer_set_header_null(void) {
    ASSERT(xylem_http_writer_set_header(NULL, "X-Foo", "bar") == -1);
}

static void test_writer_write_null(void) {
    ASSERT(xylem_http_writer_write(NULL, "data", 4) == -1);
}

static void test_upgrade_long_header(void) {
    char out[2048] = {0};
    _upgrade_transport_t fake = {
        .buf = out,
        .cap = sizeof(out),
        .max_write_len = 512,
    };
    http_transport_t transport = {
        .conn = &fake,
        .read = _upgrade_read,
        .write = _upgrade_write,
        .close = _upgrade_close,
    };
    http1_response_t response = {
        .transport = &transport,
    };
    http_writer_t writer = {
        .ops         = &http1_writer_ops,
        .impl        = &response,
        .status_code = 200,
        .state       = HTTP_WRITER_OPEN,
    };
    char value[509];
    memset(value, 'a', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';

    ASSERT(http_header_add(&writer.headers, &writer.header_count,
                           &writer.header_cap, "X-Long", 6,
                           value, strlen(value)) == 0);

    void* detached = NULL;
    ASSERT(xylem_http_writer_upgrade(&writer, &detached) == 0);
    ASSERT(detached == &transport);
    ASSERT(response.transport == NULL);
    ASSERT(!fake.oversized_write);
    ASSERT(strstr(out, "X-Long: ") != NULL);
    ASSERT(strstr(out, value) != NULL);
    ASSERT(strstr(out, "\r\n\r\n") != NULL);
    ASSERT(!fake.closed);

    http_headers_free(writer.headers, writer.header_count);
}

static void _hello_handler(xylem_http_writer_t* writer, xylem_http_req_t* req,
                           void* userdata) {
    (void)req;
    (void)userdata;
    xylem_http_writer_set_status(writer, 200);
    xylem_http_writer_set_header(writer, "Content-Type", "text/plain");
    xylem_http_writer_write(writer, "hello", 5);
}

static void _integration_body(uint16_t port) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/test", (unsigned)port);

    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 5);
    ASSERT(memcmp(xylem_http_res_body(res), "hello", 5) == 0);

    const char* ct = xylem_http_res_header(res, "Content-Type");
    ASSERT(ct != NULL);
    ASSERT(strcmp(ct, "text/plain") == 0);

    xylem_http_res_destroy(res);
}

static void test_http_integration(void) {
    _serve(_hello_handler, NULL, _integration_body);
}

static void _pool_handler(xylem_http_writer_t* writer, xylem_http_req_t* req,
                          void* userdata) {
    (void)userdata;
    const char* url = xylem_http_req_url(req);
    xylem_http_writer_set_status(writer, 200);
    xylem_http_writer_set_header(writer, "Content-Type", "text/plain");
    xylem_http_writer_write(writer, url, strlen(url));
}

static void _pool_body(uint16_t port) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/a", (unsigned)port);
    xylem_http_res_t* r1 = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(r1 != NULL);
    ASSERT(xylem_http_res_status(r1) == 200);
    ASSERT(xylem_http_res_body_len(r1) == 2);
    ASSERT(memcmp(xylem_http_res_body(r1), "/a", 2) == 0);
    xylem_http_res_destroy(r1);

    snprintf(url, sizeof(url), "http://127.0.0.1:%u/b", (unsigned)port);
    xylem_http_res_t* r2 = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(r2 != NULL);
    ASSERT(xylem_http_res_status(r2) == 200);
    ASSERT(xylem_http_res_body_len(r2) == 2);
    ASSERT(memcmp(xylem_http_res_body(r2), "/b", 2) == 0);
    xylem_http_res_destroy(r2);
}

static void test_pool_reuse(void) {
    _serve(_pool_handler, NULL, _pool_body);
}

static void _test_redirect_handler(
    xylem_http_writer_t* writer,
    xylem_http_req_t*    req,
    void*                ud) {
    (void)ud;
    const char* url = xylem_http_req_url(req);

    if (strcmp(url, "/old") == 0) {
        xylem_http_writer_set_status(writer, 301);
        xylem_http_writer_set_header(writer, "Location", "/new");
        xylem_http_writer_write(writer, "", 0);
    } else if (strcmp(url, "/new") == 0) {
        xylem_http_writer_write(writer, "arrived", 7);
    } else if (strcmp(url, "/chain1") == 0) {
        xylem_http_writer_set_status(writer, 302);
        xylem_http_writer_set_header(writer, "Location", "/chain2");
        xylem_http_writer_write(writer, "", 0);
    } else if (strcmp(url, "/chain2") == 0) {
        xylem_http_writer_set_status(writer, 302);
        xylem_http_writer_set_header(writer, "Location", "/chain3");
        xylem_http_writer_write(writer, "", 0);
    } else if (strcmp(url, "/chain3") == 0) {
        xylem_http_writer_write(writer, "end", 3);
    } else if (strcmp(url, "/see-other") == 0) {
        xylem_http_writer_set_status(writer, 303);
        xylem_http_writer_set_header(writer, "Location", "/get-only");
        xylem_http_writer_write(writer, "", 0);
    } else if (strcmp(url, "/get-only") == 0) {
        const char* method = xylem_http_req_method(req);
        xylem_http_writer_write(writer, method, strlen(method));
    } else {
        xylem_http_writer_set_status(writer, 404);
        xylem_http_writer_write(writer, "not found", 9);
    }
}

static void _redirect_body(uint16_t port) {
    char url[128];
    xylem_http_cli_opts_t opts = {0};
    opts.max_redirects = 5;

    snprintf(url, sizeof(url), "http://127.0.0.1:%u/old", (unsigned)port);
    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 7);
    ASSERT(memcmp(xylem_http_res_body(res), "arrived", 7) == 0);
    xylem_http_res_destroy(res);

    snprintf(url, sizeof(url), "http://127.0.0.1:%u/chain1", (unsigned)port);
    res = xylem_http_get(url, NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 3);
    ASSERT(memcmp(xylem_http_res_body(res), "end", 3) == 0);
    xylem_http_res_destroy(res);

    xylem_http_cli_opts_t opts_noredir = {0};
    opts_noredir.max_redirects = 0;
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/old", (unsigned)port);
    res = xylem_http_get(url, NULL, 0, &opts_noredir);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 301);
    xylem_http_res_destroy(res);

    snprintf(url, sizeof(url), "http://127.0.0.1:%u/see-other", (unsigned)port);
    res = xylem_http_post(url, "body", 4, "text/plain", NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 3);
    ASSERT(memcmp(xylem_http_res_body(res), "GET", 3) == 0);
    xylem_http_res_destroy(res);

    xylem_http_cli_opts_t opts1 = {0};
    opts1.max_redirects = 1;
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/chain1", (unsigned)port);
    res = xylem_http_get(url, NULL, 0, &opts1);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 302);
    xylem_http_res_destroy(res);
}

static void test_redirect_following(void) {
    _serve(_test_redirect_handler, NULL, _redirect_body);
}

static void _auth_handler(xylem_http_writer_t* writer, xylem_http_req_t* req,
                          void* ud) {
    (void)ud;
    const char* auth = xylem_http_req_header(req, "Authorization");
    if (!auth) {
        xylem_http_writer_set_status(writer, 401);
        xylem_http_writer_write(writer, "unauthorized", 12);
        return;
    }

    if (strncmp(auth, "Basic ", 6) != 0) {
        xylem_http_writer_set_status(writer, 401);
        xylem_http_writer_write(writer, "bad scheme", 10);
        return;
    }

    const char* b64 = auth + 6;
    xylem_http_writer_set_status(writer, 200);
    xylem_http_writer_write(writer, b64, strlen(b64));
}

static void _auth_body(uint16_t port) {
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/auth", (unsigned)port);

    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 401);
    ASSERT(xylem_http_res_body_len(res) == 12);
    ASSERT(memcmp(xylem_http_res_body(res), "unauthorized", 12) == 0);
    xylem_http_res_destroy(res);

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
}

static void test_basic_auth(void) {
    _serve(_auth_handler, NULL, _auth_body);
}

static void _expect_handler(xylem_http_writer_t* writer, xylem_http_req_t* req,
                            void* userdata) {
    (void)userdata;
    size_t blen = xylem_http_req_body_len(req);
    xylem_http_writer_set_status(writer, 200);
    xylem_http_writer_set_header(writer, "Content-Type", "text/plain");
    char buf[32];
    int  n = snprintf(buf, sizeof(buf), "%zu", blen);
    xylem_http_writer_write(writer, buf, (size_t)n);
}

static void _expect_body(uint16_t port) {
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
}

static void test_expect_continue(void) {
    _serve(_expect_handler, NULL, _expect_body);
}

static void _content_length_body(uint16_t port) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", (unsigned)port);

    xylem_http_res_t* r = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(r != NULL);
    ASSERT(xylem_http_res_status(r) == 200);
    const char* cl = xylem_http_res_header(r, "Content-Length");
    ASSERT(cl != NULL);
    ASSERT(strcmp(cl, "5") == 0);
    ASSERT(xylem_http_res_body_len(r) == 5);
    ASSERT(memcmp(xylem_http_res_body(r), "hello", 5) == 0);
    xylem_http_res_destroy(r);
}

static void test_content_length_mode(void) {
    _serve(_hello_handler, NULL, _content_length_body);
}

static void _proxy_handler(xylem_http_writer_t* writer, xylem_http_req_t* req,
                           void* userdata) {
    (void)userdata;
    const char* url = xylem_http_req_url(req);
    xylem_http_writer_set_status(writer, 200);
    xylem_http_writer_set_header(writer, "Content-Type", "text/plain");
    xylem_http_writer_write(writer, url, strlen(url));
}

static void _proxy_body(uint16_t port) {
    xylem_http_proxy_t proxy = {0};
    proxy.host = "127.0.0.1";
    proxy.port = port;

    xylem_http_cli_opts_t opts = {0};
    opts.proxy = &proxy;

    const char* target = "http://target.example/path/page";
    xylem_http_res_t* res = xylem_http_get(target, NULL, 0, &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);

    size_t blen = xylem_http_res_body_len(res);
    ASSERT(blen == strlen(target));
    ASSERT(memcmp(xylem_http_res_body(res), target, blen) == 0);
    xylem_http_res_destroy(res);
}

static void test_proxy_plain(void) {
    _serve(_proxy_handler, NULL, _proxy_body);
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_url_encode_unreserved();
    test_url_encode_reserved();
    test_url_encode_empty();
    test_url_decode_basic();
    test_url_decode_passthrough();
    test_url_encode_decode_round_trip();

    test_res_destroy_null();
    test_res_accessors_null();
    test_req_accessors_null();
    test_writer_set_status_null();
    test_writer_set_header_null();
    test_writer_write_null();
    test_upgrade_long_header();

    test_http_integration();
    test_pool_reuse();
    test_redirect_following();
    test_basic_auth();
    test_expect_continue();
    test_content_length_mode();
    test_proxy_plain();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}
