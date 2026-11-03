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
#include "net/http/http-utils.h"
#include "utils.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_OK \
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"

#define HTTP_FIXTURE_REQUEST_CAPACITY 8192

typedef struct {
    const void* response;
    size_t      response_len; /* 0 = strlen(response). */
    bool        keep_alive;
    bool        reject_body;
    bool        ignore_expect;
    uint64_t    delay_ms;
    size_t      split_at;
    char*       request;
} _http_fixture_step_t;
typedef struct {
    xylem_tcp_listener_t* listener;
    _http_fixture_step_t* steps;
    size_t                count;
    _Atomic size_t        next;
    _Atomic size_t        accepted;
    _Atomic bool          stopping;
    xylem_waitgroup_t*    wg;
    uint16_t              port;
} _http_fixture_t;
typedef struct {
    _http_fixture_t*  f;
    xylem_tcp_conn_t* conn;
} _http_fixture_conn_t;

static void _fixture_delay_expired_cb(xylem_timer_t* timer, void* userdata) {
    (void)timer;
    xylem_waitgroup_done((xylem_waitgroup_t*)userdata);
}
static void _fixture_wait_delay(uint64_t delay_ms) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_waitgroup_add(wg, 1);
    xylem_timer_t* timer =
        xylem_timer_after(delay_ms, _fixture_delay_expired_cb, wg);
    ASSERT(timer);
    xylem_waitgroup_wait(wg);
    xylem_timer_destroy(timer);
    xylem_waitgroup_destroy(wg);
}

static void _srv_fixture_conn_cb(void* arg) {
    _http_fixture_conn_t* ctx  = arg;
    _http_fixture_t*      f    = ctx->f;
    xylem_tcp_conn_t*     conn = ctx->conn;
    char                  request[32768];
    size_t                have = 0;
    for (;;) {
        size_t head = 0, wanted = 0;
        bool   sent_continue = false;
        for (;;) {
            request[have] = 0;
            char* end     = strstr(request, "\r\n\r\n");
            if (end && !head) {
                head     = (size_t)(end - request) + 4;
                wanted   = head;
                char* cl = strstr(request, "Content-Length: ");
                if (cl && cl < end) {
                    wanted += (size_t)strtoul(cl + 16, NULL, 10);
                }
                ASSERT(wanted < sizeof(request));
            }
            size_t index = atomic_load(&f->next);
            if (head && !sent_continue &&
                strstr(request, "Expect: 100-continue")) {
                ASSERT(index < f->count);
                if (f->steps[index].reject_body) {
                    wanted = head;
                } else if (!f->steps[index].ignore_expect) {
                    ASSERT(xylem_tcp_write(conn,
                                           "HTTP/1.1 100 Continue\r\n\r\n",
                                           25) == 0);
                }
                sent_continue = true;
            }
            if (head && have >= wanted) {
                break;
            }
            if (atomic_load(&f->stopping)) {
                goto done;
            }
            xylem_tcp_set_read_deadline(
                conn,
                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 2000);
            int n = xylem_tcp_read(conn,
                                   request + have,
                                   (int)(sizeof(request) - 1 - have));
            if (n <= 0) {
                goto done;
            }
            have += (size_t)n;
        }
        size_t index = atomic_fetch_add(&f->next, 1);
        ASSERT(index < f->count);
        _http_fixture_step_t* step = &f->steps[index];
        ASSERT(wanted < HTTP_FIXTURE_REQUEST_CAPACITY);
        memcpy(step->request, request, wanted);
        step->request[wanted] = 0;
        have -= wanted;
        memmove(request, request + wanted, have);
        if (step->delay_ms) {
            _fixture_wait_delay(step->delay_ms);
        }
        size_t len =
            step->response_len ? step->response_len : strlen(step->response);
        if (step->split_at && step->split_at < len) {
            if (xylem_tcp_write(conn, step->response, (int)step->split_at)) {
                goto done;
            }
            _fixture_wait_delay(5);
            if (xylem_tcp_write(conn,
                                (const char*)step->response + step->split_at,
                                (int)(len - step->split_at))) {
                goto done;
            }
        } else if (xylem_tcp_write(conn, step->response, (int)len)) {
            goto done;
        }
        if (!step->keep_alive) {
            break;
        }
    }
done:
    xylem_tcp_destroy(conn);
    free(ctx);
    xylem_waitgroup_done(f->wg);
}
static void _srv_fixture_accept_cb(void* arg) {
    _http_fixture_t* f = arg;
    while (!atomic_load(&f->stopping)) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(f->listener);
        if (!conn) {
            break;
        }
        atomic_fetch_add(&f->accepted, 1);
        _http_fixture_conn_t* c =
            (_http_fixture_conn_t*)calloc(1, sizeof(_http_fixture_conn_t));
        ASSERT(c);
        c->f    = f;
        c->conn = conn;
        xylem_waitgroup_add(f->wg, 1);
        xylem_spawn(_srv_fixture_conn_cb, c);
    }
    xylem_waitgroup_done(f->wg);
}

static void
_fixture_init(_http_fixture_t* f, _http_fixture_step_t* steps, size_t count) {
    memset(f, 0, sizeof(*f));
    f->steps = steps;
    f->count = count;
    /* Inlined test cases share a limited coroutine stack. */
    for (size_t i = 0; i < count; ++i) {
        steps[i].request = (char*)calloc(1, HTTP_FIXTURE_REQUEST_CAPACITY);
        ASSERT(steps[i].request);
    }
    atomic_init(&f->next, 0);
    atomic_init(&f->accepted, 0);
    atomic_init(&f->stopping, false);
    f->listener = xylem_tcp_listen("127.0.0.1", 0, NULL);
    ASSERT(f->listener);
    ASSERT(xylem_tcp_listener_addr(f->listener, NULL, 0, &f->port) == 0);
    f->wg = xylem_waitgroup_create();
    ASSERT(f->wg != NULL);
    xylem_waitgroup_add(f->wg, 1);
    xylem_spawn(_srv_fixture_accept_cb, f);
}
static void _fixture_deinit(_http_fixture_t* f) {
    atomic_store(&f->stopping, true);
    xylem_tcp_close_listener(f->listener);
    xylem_waitgroup_wait(f->wg);
    xylem_tcp_destroy_listener(f->listener);
    xylem_waitgroup_destroy(f->wg);
    for (size_t i = 0; i < f->count; ++i) {
        free(f->steps[i].request);
        f->steps[i].request = NULL;
    }
}
static void _fixture_url(_http_fixture_t* f, const char* path, char url[256]) {
    snprintf(url, 256, "http://127.0.0.1:%u%s", f->port, path);
}
static void _check_hello(xylem_http_res_t* r) {
    ASSERT(r);
    ASSERT(xylem_http_res_status(r) == 200);
    ASSERT(xylem_http_res_body_len(r) == 5);
    ASSERT(!memcmp(xylem_http_res_body(r), "hello", 5));
    xylem_http_res_destroy(r);
}

static void test_connection_reuse(void) {
    _http_fixture_step_t steps[] = {
        {.response   = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
         .keep_alive = true},
        {.response = HTTP_OK}};
    _http_fixture_t f;
    _fixture_init(&f, steps, 2);
    char url[256];
    _fixture_url(&f, "/", url);
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
    _fixture_init(&f, steps, 7);
    char url[256];
    _fixture_url(&f, "/v1?x=1", url);
    xylem_http_hdr_t hdr[] = {{.name = "Authorization", .value = "Bearer test"},
                              {.name = "X-Custom", .value = "value"}};
    for (int i = 0; i < 7; ++i) {
        const char*       body = (i > 0 && i < 5) ? "{}" : NULL;
        xylem_http_res_t* r =
            xylem_http_request(methods[i],
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
    _fixture_init(&f, &step, 1);
    char url[256];
    _fixture_url(&f, "/", url);
    xylem_http_res_t* res = xylem_http_get(url, NULL, 0, NULL);
    ASSERT(res);
    ASSERT(!strcmp(xylem_http_res_header(res, "X-Value"), "fragmented"));
    _check_hello(res);
    _fixture_deinit(&f);
}

static void _response_check(const char* response) {
    _http_fixture_step_t step = {.response = response};
    _http_fixture_t      f;
    _fixture_init(&f, &step, 1);
    char url[256];
    _fixture_url(&f, "/", url);
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
    _fixture_init(&f, steps, 3);
    char url[256];
    _fixture_url(&f, "/", url);
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
    _fixture_init(&dest, &target, 1);
    char next[256], response[512];
    _fixture_url(&dest, "/next", next);
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: "
             "0\r\nConnection: close\r\n\r\n",
             next);
    _http_fixture_step_t source = {.response = response};
    _http_fixture_t      origin;
    _fixture_init(&origin, &source, 1);
    char url[256];
    _fixture_url(&origin, "/start", url);
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
    _fixture_init(&f, steps, 2);
    char url[256];
    _fixture_url(&f, "/a/b/start", url);
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
    _fixture_init(&f, &step, 1);
    xylem_http_proxy_t    proxy = {.host     = "127.0.0.1",
                                   .port     = f.port,
                                   .username = "user",
                                   .password = "pass"};
    xylem_http_cli_opts_t opts  = {.proxy = &proxy, .timeout_ms = 3000};
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
    _fixture_init(&f, &step, 1);
    char url[256];
    _fixture_url(&f, "/", url);
    xylem_http_cli_opts_t opts = {.timeout_ms = 30};
    ASSERT(!xylem_http_get(url, NULL, 0, &opts));
    _fixture_deinit(&f);
}

static void test_response_body_limit(void) {
    _http_fixture_step_t  step = {.response = HTTP_OK};
    _http_fixture_t       f;
    char                  url[256];
    xylem_http_cli_opts_t opts = {0};

    _fixture_init(&f, &step, 1);
    _fixture_url(&f, "/", url);
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
    int                  n       = xylem_gzip_compress((const uint8_t*)plain,
                                                       strlen(plain),
                                                       compressed,
                                                       sizeof(compressed),
                                                       6);
    ASSERT(n > 0);
    char response[512];
    int  head =
        snprintf(response,
                 sizeof(response),
                 "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: "
                 "%d\r\nConnection: close\r\n\r\n",
                 n);
    memcpy(response + head, compressed, (size_t)n);
    step = (_http_fixture_step_t){.response     = response,
                                  .response_len = head + (size_t)n};
    _fixture_init(&f, &step, 1);
    _fixture_url(&f, "/", url);
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
    _fixture_init(&first, &a, 1);
    _fixture_init(&second, &b, 1);
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
    _fixture_init(&f, steps, 2);
    char url[256];
    _fixture_url(&f, "/", url);
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
    int     n = xylem_gzip_compress((uint8_t*)plain,
                                    sizeof(plain),
                                    compressed,
                                    sizeof(compressed),
                                    6);
    ASSERT(n > 0 && n < 128);
    char response[512];
    int  head =
        snprintf(response,
                 sizeof(response),
                 "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: "
                 "%d\r\nConnection: close\r\n\r\n",
                 n);
    memcpy(response + head, compressed, (size_t)n);
    _http_fixture_step_t step = {.response     = response,
                                 .response_len = (size_t)(head + n)};
    _fixture_init(&f, &step, 1);
    _fixture_url(&f, "/", url);
    opts = (xylem_http_cli_opts_t){.max_response_body = 128};
    ASSERT(!xylem_http_get(url, NULL, 0, &opts));
    _fixture_deinit(&f);
}

static void test_guards(void) {
    ASSERT(!xylem_http_get(NULL, NULL, 0, NULL));
    ASSERT(!xylem_http_get("ftp://example.com", NULL, 0, NULL));
    ASSERT(!xylem_http_request("BAD METHOD",
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
