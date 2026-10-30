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

#include "http-fixture.h"
#include "net/http/http-utils.h"
#include "utils.h"

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

static void test_connection_reuse(void) {
    _http_fixture_step_t steps[] = {
        {.response   = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
         .keep_alive = true},
        {.response = HTTP_OK}};
    _http_fixture_t f;
    _fixture_init(&f, steps, 2, NULL);
    char url[256];
    _fixture_url(&f, false, "/", url);
    _check_hello(xylem_http_get(url, NULL, 0, NULL));
    _check_hello(xylem_http_get(url, NULL, 0, NULL));
    ASSERT(atomic_load(&f.accepted) == 1);
    _fixture_deinit(&f);
}

static void test_request_methods(void) {
    const char* methods[] =
        {"GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "HEAD"};
    _http_fixture_step_t* steps =
        (_http_fixture_step_t*)calloc(7, sizeof(_http_fixture_step_t));
    ASSERT(steps);
    for (int i = 0; i < 7; ++i) {
        steps[i].response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
        steps[i].keep_alive = true;
    }
    steps[6].response =
        "HTTP/1.1 200 OK\r\nContent-Length: 123\r\nConnection: close\r\n\r\n";
    steps[6].keep_alive = false;
    _http_fixture_t f;
    _fixture_init(&f, steps, 7, NULL);
    char url[256];
    _fixture_url(&f, false, "/v1?x=1", url);
    xylem_http_hdr_t hdr[] = {
        {.name = "Authorization", .value = "Bearer test"},
        {.name = "X-Custom", .value = "value"}};
    for (int i = 0; i < 7; ++i) {
        const char*       body = (i > 0 && i < 5) ? "{}" : NULL;
        xylem_http_res_t* r    = xylem_http_request(
            methods[i],
            url,
            body,
            body ? 2 : 0,
            body ? "application/json" : NULL,
            hdr,
            2,
            NULL);
        if (i == 6) {
            ASSERT(r);
            ASSERT(xylem_http_res_body_len(r) == 0);
            xylem_http_res_destroy(r);
        } else {
            _check_hello(r);
        }
        ASSERT(!strncmp(steps[i].request, methods[i], strlen(methods[i])));
        ASSERT(strstr(steps[i].request, "Authorization: Bearer test"));
        ASSERT(!strstr(steps[i].request, "Expect:"));
        if (body) {
            ASSERT(strstr(steps[i].request, "\r\n\r\n{}"));
        }
    }

    _fixture_deinit(&f);
    free(steps);
}

static void test_chunked_fragmented_headers(void) {
    _http_fixture_step_t step = {
        .response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                    "X-Value: fragmented\r\nConnection: close\r\n\r\n"
                    "2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n",
        .split_at = 65};
    _http_fixture_t f;
    _fixture_init(&f, &step, 1, NULL);
    char url[256];
    _fixture_url(&f, false, "/", url);
    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(res);
    ASSERT(!strcmp(xylem_http_res_header(res, "X-Value"), "fragmented"));
    _check_hello(res);
    _fixture_deinit(&f);
}

static void _response_check(const char* response) {
    _http_fixture_step_t step = {.response = response};
    _http_fixture_t      f;
    _fixture_init(&f, &step, 1, NULL);
    char url[256];
    _fixture_url(&f, false, "/", url);
    _check_hello(xylem_http_get(url, NULL, 0, NULL));
    _fixture_deinit(&f);
}

static void test_informational_response(void) {
    _response_check("HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n" HTTP_OK);
}

static void test_close_delimited_response(void) {
    _response_check("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello");
}

static void test_expect(void) {
    _http_fixture_step_t steps[] = {
        {.response = HTTP_OK},
        {.response = HTTP_OK, .ignore_expect = true},
        {.response    = "HTTP/1.1 413 Too Large\r\nContent-Length: "
                        "0\r\nConnection: close\r\n\r\n",
         .reject_body = true},
    };
    _http_fixture_t f;
    _fixture_init(&f, steps, 3, NULL);
    char url[256];
    _fixture_url(&f, false, "/", url);
    xylem_http_cli_opts_t opts = {.timeout_ms = 3000, .expect_continue = true};
    _check_hello(
        xylem_http_post(url, "{}", 2, "application/json", NULL, 0, &opts));
    ASSERT(strstr(steps[0].request, "Expect: 100-continue"));
    _check_hello(xylem_http_post(url, "{}", 2, NULL, NULL, 0, &opts));
    xylem_http_res_t* r = xylem_http_post(url, "{}", 2, NULL, NULL, 0, &opts);
    ASSERT(r);
    ASSERT(xylem_http_res_status(r) == 413);
    xylem_http_res_destroy(r);
    ASSERT(!strstr(steps[2].request, "{}"));
    _fixture_deinit(&f);
}

static void test_redirect_credentials(void) {
    _http_fixture_step_t target = {.response = HTTP_OK};
    _http_fixture_t      dest;
    _fixture_init(&dest, &target, 1, NULL);
    char next[256], response[512];
    _fixture_url(&dest, false, "/next", next);
    snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: "
        "0\r\nConnection: close\r\n\r\n",
        next);
    _http_fixture_step_t source = {.response = response};
    _http_fixture_t      origin;
    _fixture_init(&origin, &source, 1, NULL);
    char url[256];
    _fixture_url(&origin, false, "/start", url);
    xylem_http_hdr_t hdr[] = {
        {.name = "Authorization", .value = "Bearer secret"},
        {.name = "Cookie", .value = "a=b"},
        {.name = "X-API-Key", .value = "secret"},
        {.name = "Host", .value = "custom.example"},
        {.name = "Content-Length", .value = "2"},
        {.name = "Content-Type", .value = "application/json"}};
    xylem_http_cli_opts_t opts = {.max_redirects = 5, .timeout_ms = 3000};
    _check_hello(
        xylem_http_post(url, "{}", 2, "application/json", hdr, 6, &opts));
    ASSERT(strstr(source.request, "Authorization: Bearer secret"));
    ASSERT(!strncmp(target.request, "GET /next HTTP/1.1", 18));
    ASSERT(!strstr(target.request, "secret"));
    ASSERT(!strstr(target.request, "Cookie:"));
    ASSERT(!strstr(target.request, "custom.example"));
    ASSERT(!strstr(target.request, "Content-Length: 2"));
    _fixture_deinit(&origin);
    _fixture_deinit(&dest);
}

static void test_redirect_preserves_body(void) {
    _http_fixture_step_t steps[] = {
        {.response =
             "HTTP/1.1 307 Temporary Redirect\r\nLocation: "
             "../next?ok=1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"},
        {.response = HTTP_OK},
    };
    _http_fixture_t f;
    _fixture_init(&f, steps, 2, NULL);
    char url[256];
    _fixture_url(&f, false, "/a/b/start", url);
    xylem_http_hdr_t hdr = {.name = "Authorization", .value = "Bearer keep"};
    _check_hello(
        xylem_http_put(url, "{}", 2, "application/json", &hdr, 1, NULL));
    ASSERT(strstr(steps[1].request, "PUT /a/next?ok=1 HTTP/1.1"));
    ASSERT(strstr(steps[1].request, "Authorization: Bearer keep"));
    ASSERT(strstr(steps[1].request, "\r\n\r\n{}"));
    _fixture_deinit(&f);
}

static void test_proxy(void) {
    _http_fixture_step_t step = {.response = HTTP_OK};
    _http_fixture_t      f;
    _fixture_init(&f, &step, 1, NULL);
    xylem_http_proxy_t proxy = {
        .host     = "127.0.0.1",
        .port     = f.port,
        .username = "user",
        .password = "pass"};
    xylem_http_cli_opts_t opts = {.proxy = &proxy, .timeout_ms = 3000};
    _check_hello(
        xylem_http_get("http://does-not-exist.invalid/api", NULL, 0, &opts));
    ASSERT(
        strstr(step.request, "GET http://does-not-exist.invalid/api HTTP/1.1"));
    ASSERT(strstr(step.request, "Proxy-Authorization: Basic dXNlcjpwYXNz"));
    _fixture_deinit(&f);
}

static void test_request_timeout(void) {
    _http_fixture_step_t step = {.response = HTTP_OK, .delay_ms = 150};
    _http_fixture_t      f;
    _fixture_init(&f, &step, 1, NULL);
    char url[256];
    _fixture_url(&f, false, "/", url);
    xylem_http_cli_opts_t opts = {.timeout_ms = 30};
    ASSERT(!xylem_http_get(url, NULL, 0, &opts));
    _fixture_deinit(&f);
}

static void test_response_body_limit(void) {
    _http_fixture_step_t  step = {.response = HTTP_OK};
    _http_fixture_t       f;
    char                  url[256];
    xylem_http_cli_opts_t opts = {0};

    _fixture_init(&f, &step, 1, NULL);
    _fixture_url(&f, false, "/", url);
    opts = (xylem_http_cli_opts_t){.max_response_body = 4};
    ASSERT(!xylem_http_get(url, NULL, 0, &opts));
    _fixture_deinit(&f);
}

static void test_gzip_response(void) {
    _http_fixture_step_t step = {0};
    _http_fixture_t      f;
    char                 url[256];
    uint8_t              compressed[256];
    const char           plain[] = "hello hello hello hello hello";
    int                  n       = xylem_gzip_compress(
        (const uint8_t*)plain,
        strlen(plain),
        compressed,
        sizeof(compressed),
        6);
    ASSERT(n > 0);
    char response[512];
    int  head = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: "
        "%d\r\nConnection: close\r\n\r\n",
        n);
    memcpy(response + head, compressed, (size_t)n);
    step = (_http_fixture_step_t){
        .response     = response,
        .response_len = head + (size_t)n};
    _fixture_init(&f, &step, 1, NULL);
    _fixture_url(&f, false, "/", url);
    xylem_http_res_t* r = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(r);
    ASSERT(xylem_http_res_body_len(r) == strlen(plain));
    ASSERT(!memcmp(xylem_http_res_body(r), plain, strlen(plain)));
    xylem_http_res_destroy(r);
    _fixture_deinit(&f);
}

static void test_proxy_pool_isolation(void) {
    _http_fixture_step_t a = {
        .response   = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
        .keep_alive = true};
    _http_fixture_step_t b = {.response = HTTP_OK};
    _http_fixture_t      first, second;
    _fixture_init(&first, &a, 1, NULL);
    _fixture_init(&second, &b, 1, NULL);
    xylem_http_proxy_t    one  = {.host = "127.0.0.1", .port = first.port};
    xylem_http_proxy_t    two  = {.host = "127.0.0.1", .port = second.port};
    xylem_http_cli_opts_t opts = {.proxy = &one, .timeout_ms = 1000};
    _check_hello(xylem_http_get("http://example.invalid/api", NULL, 0, &opts));
    opts.proxy = &two;
    _check_hello(xylem_http_get("http://example.invalid/api", NULL, 0, &opts));
    ASSERT(atomic_load(&first.next) == 1 && atomic_load(&second.next) == 1);
    xylem_http_pool_clear();
    _fixture_deinit(&first);
    _fixture_deinit(&second);
}

static void test_redirect_total_deadline(void) {
    _http_fixture_step_t steps[] = {
        {.response = "HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: "
                     "0\r\nConnection: close\r\n\r\n",
         .delay_ms = 35},
        {.response = HTTP_OK, .delay_ms = 35},
    };
    _http_fixture_t f;
    _fixture_init(&f, steps, 2, NULL);
    char url[256];
    _fixture_url(&f, false, "/", url);
    xylem_http_cli_opts_t opts = {.timeout_ms = 50, .max_redirects = 5};
    ASSERT(!xylem_http_get(url, NULL, 0, &opts));
    _fixture_deinit(&f);
}

static void test_gzip_expansion_limit(void) {
    _http_fixture_t       f;
    char                  url[256];
    xylem_http_cli_opts_t opts = {0};
    char                  plain[4096];
    memset(plain, 'A', sizeof(plain));
    uint8_t compressed[256];
    int     n = xylem_gzip_compress(
        (uint8_t*)plain,
        sizeof(plain),
        compressed,
        sizeof(compressed),
        6);
    ASSERT(n > 0 && n < 128);
    char response[512];
    int  head = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: "
        "%d\r\nConnection: close\r\n\r\n",
        n);
    memcpy(response + head, compressed, (size_t)n);
    _http_fixture_step_t step = {
        .response     = response,
        .response_len = (size_t)(head + n)};
    _fixture_init(&f, &step, 1, NULL);
    _fixture_url(&f, false, "/", url);
    opts = (xylem_http_cli_opts_t){.max_response_body = 128};
    ASSERT(!xylem_http_get(url, NULL, 0, &opts));
    _fixture_deinit(&f);
}

static void test_guards(void) {
    ASSERT(!xylem_http_get(NULL, NULL, 0, NULL));
    ASSERT(!xylem_http_get("ftp://example.com", NULL, 0, NULL));
    ASSERT(!xylem_http_request(
        "BAD METHOD",
        "http://127.0.0.1",
        NULL,
        0,
        NULL,
        NULL,
        0,
        NULL));
    xylem_http_hdr_t bad = {.name = "X-Test", .value = "ok\r\nInjected: bad"};
    ASSERT(!xylem_http_get("http://127.0.0.1", &bad, 1, NULL));
    http_url_t parsed;
    ASSERT(!http_url_parse("http://[::1]:8080/x", &parsed));
    char url[256];
    ASSERT(!http_url_serialize(&parsed, url, sizeof(url)));
    ASSERT(!strcmp(url, "http://[::1]:8080/x"));
}

static void _test_run_cb(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_proxy_pool_isolation();
    test_redirect_total_deadline();
    test_gzip_expansion_limit();
    test_guards();

    test_request_methods();
    test_connection_reuse();

    test_chunked_fragmented_headers();
    test_informational_response();
    test_close_delimited_response();

    test_expect();

    test_redirect_credentials();

    test_redirect_preserves_body();

    test_proxy();

    test_request_timeout();
    test_response_body_limit();
    test_gzip_response();
    xylem_http_pool_clear();
    puts("All HTTP client tests passed.");
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    xylem_run(_test_run_cb, NULL, NULL);
    return 0;
}
