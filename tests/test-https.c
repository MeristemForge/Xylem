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

#define TEST_WITH_TLS

#include "http-fixture.h"
#include "utils.h"

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#define HTTPS_CERT "test_https_cert.pem"
#define HTTPS_KEY  "test_https_key.pem"

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
    snprintf(
        response,
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
    xylem_http_cli_opts_t opts = {
        .tls           = &tls,
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
    snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: "
        "0\r\nConnection: close\r\n\r\n",
        next);
    _http_fixture_step_t steps[] = {
        {.response = response},
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
        int n = xylem_tcp_read(
            client,
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
    ASSERT(
        xylem_tcp_write(
            client,
            "HTTP/1.1 200 Connection Established\r\n\r\n",
            39) == 0);
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
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
    xylem_waitgroup_add(proxy.wg, 1);
    xylem_spawn(_srv_tunnel_proxy_cb, &proxy);
    xylem_http_proxy_t config = {
        .host     = "127.0.0.1",
        .port     = port,
        .username = "user",
        .password = "pass"};
    xylem_http_tls_t      tls  = {.ca = HTTPS_CERT};
    xylem_http_cli_opts_t opts = {
        .proxy      = &config,
        .tls        = &tls,
        .timeout_ms = 3000};
    char url[256];
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
