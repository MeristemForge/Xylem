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

/* IPv4 pton + ntop round-trip */
static void test_ipv4_roundtrip(void) {
    xylem_addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(xylem_addr_pton("127.0.0.1", 8080, &addr) == 0);
    ASSERT(xylem_addr_ntop(&addr, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "127.0.0.1") == 0);
    ASSERT(port == 8080);
}

/* IPv6 pton + ntop round-trip */
static void test_ipv6_roundtrip(void) {
    xylem_addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(xylem_addr_pton("::1", 9090, &addr) == 0);
    ASSERT(xylem_addr_ntop(&addr, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "::1") == 0);
    ASSERT(port == 9090);
}

/* Invalid address returns -1 */
static void test_invalid_address(void) {
    xylem_addr_t addr;
    ASSERT(xylem_addr_pton("not_an_address", 80, &addr) == -1);
    ASSERT(xylem_addr_pton("999.999.999.999", 80, &addr) == -1);
}

/* NULL parameter handling */
static void test_null_params(void) {
    xylem_addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(xylem_addr_pton(NULL, 80, &addr) == -1);
    ASSERT(xylem_addr_pton("127.0.0.1", 80, NULL) == -1);
    ASSERT(xylem_addr_ntop(NULL, host, sizeof(host), &port) == -1);
    ASSERT(xylem_addr_ntop(&addr, NULL, 0, &port) == -1);
}

/* IPv4 wildcard address */
static void test_ipv4_wildcard(void) {
    xylem_addr_t addr;
    char host[64];
    uint16_t port;

    ASSERT(xylem_addr_pton("0.0.0.0", 0, &addr) == 0);
    ASSERT(xylem_addr_ntop(&addr, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "0.0.0.0") == 0);
    ASSERT(port == 0);
}

/**
 * Context for async resolve tests. Passed via userdata to the resolve
 * callback, and via file-scope pointer to timer callbacks (timer has
 * no ud field).
 */
typedef struct {
    xylem_loop_t          loop;
    xylem_thrdpool_t*     pool;
    int                   status;
    size_t                count;
    const char*           host;
    xylem_addr_resolve_fn_t resolve_cb;
    xylem_loop_timer_t    keepalive;
} _resolve_ctx_t;

static _resolve_ctx_t* _ctx;

static void _on_resolved(xylem_addr_t* addrs, size_t count,
                         int status, void* userdata) {
    _resolve_ctx_t* ctx = userdata;
    ctx->status = status;
    ctx->count  = count;

    for (size_t i = 0; i < count; i++) {
        ASSERT(addrs[i].storage.ss_family == AF_INET ||
               addrs[i].storage.ss_family == AF_INET6);
    }

    xylem_loop_deinit_timer(&ctx->keepalive);
    xylem_loop_stop(&ctx->loop);
}

static void _on_resolve_fail(xylem_addr_t* addrs, size_t count,
                             int status, void* userdata) {
    (void)addrs;
    _resolve_ctx_t* ctx = userdata;
    ctx->status = status;
    ctx->count  = count;
    xylem_loop_deinit_timer(&ctx->keepalive);
    xylem_loop_stop(&ctx->loop);
}

static void _keepalive_cb(xylem_loop_t* loop, xylem_loop_timer_t* timer) {
    (void)loop;
    (void)timer;
}

static void _start_resolve_cb(xylem_loop_t* loop,
                               xylem_loop_timer_t* timer) {
    (void)loop;
    xylem_loop_deinit_timer(timer);

    /* Keep the loop alive until the resolve callback fires. */
    xylem_loop_init_timer(&_ctx->loop, &_ctx->keepalive);
    xylem_loop_start_timer(&_ctx->keepalive, _keepalive_cb, 30000, 0);

    xylem_addr_resolve(&_ctx->loop, _ctx->pool, _ctx->host, 80,
                       _ctx->resolve_cb, _ctx);
}

/* Resolve localhost asynchronously - success path. */
static void test_resolve_localhost(void) {
    _resolve_ctx_t ctx = {0};
    ctx.status     = -1;
    ctx.count      = 0;
    ctx.host       = "localhost";
    ctx.resolve_cb = _on_resolved;
    _ctx = &ctx;

    xylem_loop_init(&ctx.loop);
    ctx.pool = xylem_thrdpool_create(1);

    xylem_loop_timer_t timer;
    xylem_loop_init_timer(&ctx.loop, &timer);
    xylem_loop_start_timer(&timer, _start_resolve_cb, 0, 0);

    xylem_loop_run(&ctx.loop);

    ASSERT(ctx.status == 0);
    ASSERT(ctx.count > 0);

    xylem_thrdpool_destroy(ctx.pool);
    xylem_loop_deinit(&ctx.loop);
}

/* Resolve non-existent host - error path. */
static void test_resolve_fail(void) {
    _resolve_ctx_t ctx = {0};
    ctx.status     = 0;
    ctx.count      = 99;
    ctx.host       = "this.host.does.not.exist.invalid";
    ctx.resolve_cb = _on_resolve_fail;
    _ctx = &ctx;

    xylem_loop_init(&ctx.loop);
    ctx.pool = xylem_thrdpool_create(1);

    xylem_loop_timer_t timer;
    xylem_loop_init_timer(&ctx.loop, &timer);
    xylem_loop_start_timer(&timer, _start_resolve_cb, 0, 0);

    xylem_loop_run(&ctx.loop);

    ASSERT(ctx.status == -1);
    ASSERT(ctx.count == 0);

    xylem_thrdpool_destroy(ctx.pool);
    xylem_loop_deinit(&ctx.loop);
}

/* Resolve a public hostname - verifies real DNS works. */
static void test_resolve_remote(void) {
    _resolve_ctx_t ctx = {0};
    ctx.status     = -1;
    ctx.count      = 0;
    ctx.host       = "www.baidu.com";
    ctx.resolve_cb = _on_resolved;
    _ctx = &ctx;

    xylem_loop_init(&ctx.loop);
    ctx.pool = xylem_thrdpool_create(1);

    xylem_loop_timer_t timer;
    xylem_loop_init_timer(&ctx.loop, &timer);
    xylem_loop_start_timer(&timer, _start_resolve_cb, 0, 0);

    xylem_loop_run(&ctx.loop);

    ASSERT(ctx.status == 0);
    ASSERT(ctx.count > 0);

    xylem_thrdpool_destroy(ctx.pool);
    xylem_loop_deinit(&ctx.loop);
}

/* NULL parameters return NULL. */
static void test_resolve_null_params(void) {
    xylem_loop_t loop;
    xylem_loop_init(&loop);
    xylem_thrdpool_t* pool = xylem_thrdpool_create(1);

    ASSERT(xylem_addr_resolve(NULL, pool, "localhost", 80,
                              _on_resolved, NULL) == NULL);
    ASSERT(xylem_addr_resolve(&loop, NULL, "localhost", 80,
                              _on_resolved, NULL) == NULL);
    ASSERT(xylem_addr_resolve(&loop, pool, NULL, 80,
                              _on_resolved, NULL) == NULL);
    ASSERT(xylem_addr_resolve(&loop, pool, "localhost", 80,
                              NULL, NULL) == NULL);

    xylem_thrdpool_destroy(pool);
    xylem_loop_deinit(&loop);
}

int main(void) {
    xylem_startup();

    test_ipv4_roundtrip();
    test_ipv6_roundtrip();
    test_invalid_address();
    test_null_params();
    test_ipv4_wildcard();
    test_resolve_localhost();
    test_resolve_remote();
    test_resolve_fail();
    test_resolve_null_params();

    xylem_cleanup();
    return 0;
}
