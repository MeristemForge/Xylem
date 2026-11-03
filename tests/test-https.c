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
#define TEST_WITH_TLS
#include "utils.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_OK \
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"

#define HTTP_FIXTURE_REQUEST_CAPACITY 8192

#define HTTPS_CERT "test_https_cert.pem"
#define HTTPS_KEY  "test_https_key.pem"

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
static void _fixture_prepare(
    _http_fixture_t*      f,
    _http_fixture_step_t* steps,
    size_t                count) {
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
}

static void _fixture_start(_http_fixture_t* f) {
    f->wg = xylem_waitgroup_create();
    ASSERT(f->wg != NULL);
    xylem_waitgroup_add(f->wg, 1);
    xylem_spawn(_srv_fixture_accept_cb, f);
}

static void _fixture_init(
    _http_fixture_t* f,
    _http_fixture_step_t* steps,
    size_t count) {
    _fixture_prepare(f, steps, count);
    f->listener = xylem_tcp_listen("127.0.0.1", 0, NULL);
    f->accept   = _fixture_accept;
    f->close    = _fixture_close;
    f->destroy  = _fixture_destroy;
    ASSERT(f->listener);
    ASSERT(xylem_tcp_listener_addr(f->listener, NULL, 0, &f->port) == 0);
    _fixture_start(f);
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
static void _fixture_url(
    _http_fixture_t* f,
    bool secure,
    const char* path,
    char url[256]) {
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

static void _fixture_secure_init(
    _http_fixture_t*      f,
    _http_fixture_step_t* steps,
    size_t                count,
    xylem_tls_ctx_t*      tls) {
    if (!tls) {
        _fixture_init(f, steps, count);
        return;
    }
    _fixture_prepare(f, steps, count);
    f->listener = xylem_tls_listen("127.0.0.1", 0, tls, NULL);
    f->accept   = _fixture_tls_accept;
    f->close    = _fixture_tls_close;
    f->destroy  = _fixture_tls_destroy;
    ASSERT(f->listener);
    ASSERT(xylem_tls_listener_addr(f->listener, NULL, 0, &f->port) == 0);
    _fixture_start(f);
}

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
    _fixture_secure_init(&f, steps, 2, ctx);
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
    _fixture_secure_init(&secure, &target, 1, ctx);
    char next[256], response[512];
    _fixture_url(&secure, true, "/secure", next);
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: "
             "0\r\nConnection: close\r\n\r\n",
             next);
    _http_fixture_step_t step = {.response = response};
    _http_fixture_t      plain;
    _fixture_secure_init(&plain, &step, 1, NULL);
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
    _fixture_secure_init(&plain, &target, 1, NULL);
    _fixture_url(&plain, false, "/plain", next);
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: "
             "0\r\nConnection: close\r\n\r\n",
             next);
    _http_fixture_step_t steps[] = {{.response = response},
                                    {.response = response}};
    _fixture_secure_init(&secure, steps, 2, ctx);
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
    _fixture_secure_init(&server, &step, 1, ctx);
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

static void _test_run_cb(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    test_https_and_pool_policy();
    test_cross_scheme_redirects();
    test_connect_proxy();
    test_bad_configuration();
    xylem_http_pool_clear();
    puts("All HTTPS client tests passed.");
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    xylem_run(_test_run_cb, NULL, NULL);
    return 0;
}
