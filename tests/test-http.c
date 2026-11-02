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

#include "net/http/http.h"
#include "net/http/http-utils.h"
#include "utils.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_OK \
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"

#define HTTP_FIXTURE_REQUEST_CAPACITY 8192

/* Test-only scripted HTTP peer over the public TCP/TLS APIs. */

#ifdef TEST_WITH_TLS
#define HTTPS_CERT "test_https_cert.pem"
#define HTTPS_KEY  "test_https_key.pem"
#endif

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
    void* listener;
    http_transport_t (*accept)(void*);
    void (*close)(void*);
    void (*destroy)(void*);
    _http_fixture_step_t* steps;
    size_t                count;
    _Atomic size_t        next;
    _Atomic size_t        accepted;
    _Atomic bool          stopping;
    xylem_waitgroup_t*    wg;
    uint16_t              port;
} _http_fixture_t;
typedef struct {
    _http_fixture_t* f;
    http_transport_t t;
} _http_fixture_conn_t;

#ifdef TEST_WITH_TLS
typedef struct {
    xylem_tcp_conn_t*  from;
    xylem_tcp_conn_t*  to;
    xylem_waitgroup_t* wg;
} _tunnel_copy_t;

typedef struct {
    xylem_tcp_listener_t* listener;
    uint16_t              target_port;
    xylem_waitgroup_t*    wg;
    char                  request[2048];
} _tunnel_proxy_t;
#endif

static int _fixture_read(void* p, void* b, int n) {
    return xylem_tcp_read(p, b, n);
}
static int _fixture_write(void* p, const void* b, int n) {
    return xylem_tcp_write(p, b, n);
}
static void _fixture_destroy_conn(void* p) {
    xylem_tcp_destroy(p);
}
static void _fixture_rd(void* p, uint64_t ms) {
    xylem_tcp_set_read_deadline(p, ms);
}
static http_transport_t _fixture_accept(void* p) {
    xylem_tcp_conn_t* c = xylem_tcp_accept(p);
    return (http_transport_t){.conn            = c,
                              .read            = _fixture_read,
                              .write           = _fixture_write,
                              .close           = _fixture_destroy_conn,
                              .set_rd_deadline = _fixture_rd};
}
static void _fixture_close(void* p) {
    xylem_tcp_close_listener(p);
}
static void _fixture_destroy(void* p) {
    xylem_tcp_destroy_listener(p);
}
#ifdef TEST_WITH_TLS
static int _fixture_tls_read(void* p, void* b, int n) {
    return xylem_tls_read(p, b, n);
}
static int _fixture_tls_write(void* p, const void* b, int n) {
    return xylem_tls_write(p, b, n);
}
static void _fixture_tls_destroy_conn(void* p) {
    xylem_tls_destroy(p);
}
static void _fixture_tls_rd(void* p, uint64_t ms) {
    xylem_tls_set_read_deadline(p, ms);
}
static http_transport_t _fixture_tls_accept(void* p) {
    xylem_tls_conn_t* c = xylem_tls_accept(p);
    return (http_transport_t){.conn            = c,
                              .read            = _fixture_tls_read,
                              .write           = _fixture_tls_write,
                              .close           = _fixture_tls_destroy_conn,
                              .set_rd_deadline = _fixture_tls_rd};
}
static void _fixture_tls_close(void* p) {
    xylem_tls_close_listener(p);
}
static void _fixture_tls_destroy(void* p) {
    xylem_tls_destroy_listener(p);
}
#endif

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
    _http_fixture_conn_t* ctx = arg;
    _http_fixture_t*      f   = ctx->f;
    http_transport_t      t   = ctx->t;
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
                    ASSERT(t.write(t.conn,
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
            t.set_rd_deadline(t.conn,
                              xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) +
                                  2000);
            int n = t.read(t.conn,
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
            if (t.write(t.conn, step->response, (int)step->split_at)) {
                goto done;
            }
            _fixture_wait_delay(5);
            if (t.write(t.conn,
                        (const char*)step->response + step->split_at,
                        (int)(len - step->split_at))) {
                goto done;
            }
        } else if (t.write(t.conn, step->response, (int)len)) {
            goto done;
        }
        if (!step->keep_alive) {
            break;
        }
    }
done:
    t.close(t.conn);
    free(ctx);
    xylem_waitgroup_done(f->wg);
}
static void _srv_fixture_accept_cb(void* arg) {
    _http_fixture_t* f = arg;
    while (!atomic_load(&f->stopping)) {
        http_transport_t t = f->accept(f->listener);
        if (!t.conn) {
            break;
        }
        atomic_fetch_add(&f->accepted, 1);
        _http_fixture_conn_t* c =
            (_http_fixture_conn_t*)calloc(1, sizeof(_http_fixture_conn_t));
        ASSERT(c);
        c->f = f;
        c->t = t;
        xylem_waitgroup_add(f->wg, 1);
        xylem_spawn(_srv_fixture_conn_cb, c);
    }
    xylem_waitgroup_done(f->wg);
}
static void _fixture_init(_http_fixture_t*      f,
                          _http_fixture_step_t* steps,
                          size_t                count,
                          xylem_tls_ctx_t*      tls) {
    (void)tls;
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
#ifdef TEST_WITH_TLS
    if (tls) {
        f->listener = xylem_tls_listen("127.0.0.1", 0, tls, NULL);
        f->accept   = _fixture_tls_accept;
        f->close    = _fixture_tls_close;
        f->destroy  = _fixture_tls_destroy;
        ASSERT(f->listener);
        ASSERT(xylem_tls_listener_addr(f->listener, NULL, 0, &f->port) == 0);
    }
#endif
    if (!f->listener) {
        f->listener = xylem_tcp_listen("127.0.0.1", 0, NULL);
        f->accept   = _fixture_accept;
        f->close    = _fixture_close;
        f->destroy  = _fixture_destroy;
        ASSERT(f->listener);
        ASSERT(xylem_tcp_listener_addr(f->listener, NULL, 0, &f->port) == 0);
    }
    f->wg = xylem_waitgroup_create();
    ASSERT(f->wg != NULL);
    xylem_waitgroup_add(f->wg, 1);
    xylem_spawn(_srv_fixture_accept_cb, f);
}
static void _fixture_deinit(_http_fixture_t* f) {
    atomic_store(&f->stopping, true);
    f->close(f->listener);
    xylem_waitgroup_wait(f->wg);
    f->destroy(f->listener);
    xylem_waitgroup_destroy(f->wg);
    for (size_t i = 0; i < f->count; ++i) {
        free(f->steps[i].request);
        f->steps[i].request = NULL;
    }
}
static void
_fixture_url(_http_fixture_t* f, bool secure, const char* path, char url[256]) {
    snprintf(url,
             256,
             "%s://127.0.0.1:%u%s",
             secure ? "https" : "http",
             f->port,
             path);
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
    snprintf(response,
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
    _fixture_init(&f, &step, 1, NULL);
    _fixture_url(&f, false, "/", url);
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

#ifdef TEST_WITH_TLS
static xylem_tls_ctx_t* _srv_create_ctx(void) {
    ASSERT(_utils_cert_gen(HTTPS_CERT, HTTPS_KEY) == 0);
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx);
    ASSERT(xylem_tls_ctx_load_cert(ctx, NULL, HTTPS_CERT, HTTPS_KEY) == 0);
    return ctx;
}

static void test_https_and_pool_policy(void) {
    xylem_tls_ctx_t*     ctx     = _srv_create_ctx();
    _http_fixture_step_t steps[] = {
        {.response   = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
         .keep_alive = true},
        {.response = HTTP_OK}};
    _http_fixture_t f;
    _fixture_init(&f, steps, 2, ctx);
    char url[256];
    _fixture_url(&f, true, "/api", url);
    xylem_http_tls_t      insecure = {.skip_verify = true};
    xylem_http_cli_opts_t opts     = {.tls = &insecure, .timeout_ms = 3000};
    _check_hello(
        xylem_http_post(url, "{}", 2, "application/json", NULL, 0, &opts));
    /* A strict call must not reuse an insecure connection. */
    opts.tls = NULL;
    ASSERT(!xylem_http_get(url, NULL, 0, &opts));
    xylem_http_tls_t verified = {.ca = HTTPS_CERT};
    opts.tls                  = &verified;
    _check_hello(xylem_http_get(url, NULL, 0, &opts));
    ASSERT(atomic_load(&f.accepted) >= 3);
    xylem_http_pool_clear();
    _fixture_deinit(&f);
    xylem_tls_ctx_destroy(ctx);
    remove(HTTPS_CERT);
    remove(HTTPS_KEY);
}

static void test_cross_scheme_redirects(void) {
    xylem_tls_ctx_t*     ctx    = _srv_create_ctx();
    _http_fixture_step_t target = {.response = HTTP_OK};
    _http_fixture_t      secure;
    _fixture_init(&secure, &target, 1, ctx);
    char next[256], response[512];
    _fixture_url(&secure, true, "/secure", next);
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: "
             "0\r\nConnection: close\r\n\r\n",
             next);
    _http_fixture_step_t step = {.response = response};
    _http_fixture_t      plain;
    _fixture_init(&plain, &step, 1, NULL);
    char url[256];
    _fixture_url(&plain, false, "/start", url);
    xylem_http_tls_t      tls  = {.ca = HTTPS_CERT};
    xylem_http_cli_opts_t opts = {.tls           = &tls,
                                  .timeout_ms    = 3000,
                                  .max_redirects = 5};
    xylem_http_hdr_t hdr = {.name = "Authorization", .value = "Bearer secret"};
    _check_hello(xylem_http_get(url, &hdr, 1, &opts));
    ASSERT(!strstr(target.request, "Authorization:"));
    _fixture_deinit(&plain);
    _fixture_deinit(&secure);

    target = (_http_fixture_step_t){.response = HTTP_OK};
    _fixture_init(&plain, &target, 1, NULL);
    _fixture_url(&plain, false, "/plain", next);
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: "
             "0\r\nConnection: close\r\n\r\n",
             next);
    _http_fixture_step_t steps[] = {{.response = response},
                                    {.response = response}};
    _fixture_init(&secure, steps, 2, ctx);
    _fixture_url(&secure, true, "/start", url);
    ASSERT(!xylem_http_get(url, &hdr, 1, &opts));
    ASSERT(atomic_load(&plain.next) == 0);
    opts.allow_https_downgrade = true;
    _check_hello(xylem_http_get(url, &hdr, 1, &opts));
    ASSERT(!strstr(target.request, "Authorization:"));
    _fixture_deinit(&secure);
    _fixture_deinit(&plain);
    xylem_tls_ctx_destroy(ctx);
    remove(HTTPS_CERT);
    remove(HTTPS_KEY);
}

static void _srv_tunnel_copy_cb(void* arg) {
    _tunnel_copy_t* c = arg;
    char            buf[4096];
    int             n;
    while ((n = xylem_tcp_read(c->from, buf, sizeof(buf))) > 0) {
        if (xylem_tcp_write(c->to, buf, n)) {
            break;
        }
    }
    xylem_tcp_shutdown_wr(c->to);
    xylem_waitgroup_done(c->wg);
}

static void _srv_tunnel_proxy_cb(void* arg) {
    _tunnel_proxy_t*  p      = arg;
    xylem_tcp_conn_t* client = xylem_tcp_accept(p->listener);
    ASSERT(client);
    size_t have = 0;
    for (;;) {
        int n = xylem_tcp_read(client,
                               p->request + have,
                               (int)(sizeof(p->request) - have - 1));
        ASSERT(n > 0);
        have += (size_t)n;
        p->request[have] = 0;
        if (strstr(p->request, "\r\n\r\n")) {
            break;
        }
    }
    ASSERT(!strncmp(p->request, "CONNECT ", 8));
    xylem_tcp_conn_t* target =
        xylem_tcp_dial("127.0.0.1", p->target_port, NULL);
    ASSERT(target);
    ASSERT(xylem_tcp_write(client,
                           "HTTP/1.1 200 Connection Established\r\n\r\n",
                           39) == 0);
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_waitgroup_add(wg, 2);
    _tunnel_copy_t up   = {.from = client, .to = target, .wg = wg};
    _tunnel_copy_t down = {.from = target, .to = client, .wg = wg};
    xylem_spawn(_srv_tunnel_copy_cb, &up);
    _srv_tunnel_copy_cb(&down);
    xylem_waitgroup_wait(wg);
    xylem_waitgroup_destroy(wg);
    xylem_tcp_destroy(client);
    xylem_tcp_destroy(target);
    xylem_waitgroup_done(p->wg);
}

static void test_connect_proxy(void) {
    xylem_tls_ctx_t*     ctx  = _srv_create_ctx();
    _http_fixture_step_t step = {.response = HTTP_OK};
    _http_fixture_t      server;
    _fixture_init(&server, &step, 1, ctx);
    _tunnel_proxy_t proxy = {.target_port = server.port};
    proxy.listener        = xylem_tcp_listen("127.0.0.1", 0, NULL);
    ASSERT(proxy.listener);
    uint16_t port;
    ASSERT(xylem_tcp_listener_addr(proxy.listener, NULL, 0, &port) == 0);
    proxy.wg = xylem_waitgroup_create();
    ASSERT(proxy.wg != NULL);
    xylem_waitgroup_add(proxy.wg, 1);
    xylem_spawn(_srv_tunnel_proxy_cb, &proxy);
    xylem_http_proxy_t    config = {.host     = "127.0.0.1",
                                    .port     = port,
                                    .username = "user",
                                    .password = "pass"};
    xylem_http_tls_t      tls    = {.ca = HTTPS_CERT};
    xylem_http_cli_opts_t opts   = {.proxy      = &config,
                                    .tls        = &tls,
                                    .timeout_ms = 3000};
    char                  url[256];
    _fixture_url(&server, true, "/api", url);
    _check_hello(xylem_http_get(url, NULL, 0, &opts));
    xylem_waitgroup_wait(proxy.wg);
    ASSERT(strstr(proxy.request, "Proxy-Authorization: Basic dXNlcjpwYXNz"));
    ASSERT(!strstr(step.request, "Proxy-Authorization:"));
    xylem_tcp_destroy_listener(proxy.listener);
    xylem_waitgroup_destroy(proxy.wg);
    _fixture_deinit(&server);
    xylem_tls_ctx_destroy(ctx);
    remove(HTTPS_CERT);
    remove(HTTPS_KEY);
}

static void test_bad_configuration(void) {
    xylem_http_tls_t      tls  = {.ca = "missing-xylem-ca.pem"};
    xylem_http_cli_opts_t opts = {.tls = &tls, .timeout_ms = 100};
    ASSERT(!xylem_http_get("https://127.0.0.1:1", NULL, 0, &opts));
}
#endif

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
#ifdef TEST_WITH_TLS
    test_https_and_pool_policy();
    test_cross_scheme_redirects();
    test_connect_proxy();
    test_bad_configuration();
    xylem_http_pool_clear();
    puts("All HTTP/HTTPS client tests passed.");
#else
    puts("All HTTP client tests passed.");
#endif
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    xylem_run(_test_run_cb, NULL, NULL);
    return 0;
}
