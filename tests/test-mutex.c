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
#include "runtime/runtime.h"
#include "assert.h"

#include <stdatomic.h>
#include <stdio.h>

#define SAFETY_TIMEOUT_MS 5000

static xylem_runtime_opts_t _rt_opts = { .workers = 4 };

static void _safety_timeout_cb(
    loop_t* loop, loop_timer_t* timer, void* ud) {
    (void)loop; (void)ud;
    loop_stop_timer(timer);
    loop_destroy_timer(timer);
    xylem_runtime_stop();
    ASSERT(0 && "test timed out");
}

static void _safety_timer_post_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)req; (void)ud;
    loop_timer_t* t = loop_create_timer(loop);
    loop_start_timer(t, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);
}

static void _start_safety_timer(void) {
    loop_post(runtime_get_loop(), _safety_timer_post_cb, NULL);
}

#define MTX_WORKERS    20
#define MTX_INCREMENTS 100

typedef struct {
    xylem_mutex_t* mtx;
    int            counter;
    atomic_int     finished;
    int            tested;
} _mtx_ctx_t;

static void _mtx_worker(void* arg) {
    _mtx_ctx_t* ctx = (_mtx_ctx_t*)arg;
    for (int i = 0; i < MTX_INCREMENTS; i++) {
        xylem_mutex_lock(ctx->mtx);
        ctx->counter++;
        xylem_mutex_unlock(ctx->mtx);
    }
    int prev = atomic_fetch_add(&ctx->finished, 1);
    if (prev == MTX_WORKERS - 1) {
        ASSERT(ctx->counter == MTX_WORKERS * MTX_INCREMENTS);
        ctx->tested = 1;
        xylem_runtime_stop();
    }
}

static void _test_mtx_main(void* arg) {
    _mtx_ctx_t* ctx = (_mtx_ctx_t*)arg;
    _start_safety_timer();
    ctx->mtx = xylem_mutex_create();
    for (int i = 0; i < MTX_WORKERS; i++) {
        xylem_runtime_spawn(_mtx_worker, ctx);
    }
}

static void test_mutex_concurrent(void) {
    fprintf(stderr, "=== test_mutex_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _mtx_ctx_t ctx = {0};
        xylem_runtime_start(_test_mtx_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
        xylem_mutex_destroy(ctx.mtx);
    }
}

#define MTX_PING_PONG 200

typedef struct {
    xylem_mutex_t* mtx;
    atomic_int     value;
    atomic_int     finished;
    int            tested;
} _mtx_pp_ctx_t;

static void _mtx_ping(void* arg) {
    _mtx_pp_ctx_t* ctx = (_mtx_pp_ctx_t*)arg;
    for (int i = 0; i < MTX_PING_PONG; i++) {
        xylem_mutex_lock(ctx->mtx);
        atomic_fetch_add(&ctx->value, 1);
        xylem_mutex_unlock(ctx->mtx);
    }
    int prev = atomic_fetch_add(&ctx->finished, 1);
    if (prev == 1) {
        ASSERT(atomic_load(&ctx->value) == MTX_PING_PONG * 2);
        ctx->tested = 1;
        xylem_runtime_stop();
    }
}

static void _mtx_pong(void* arg) {
    _mtx_pp_ctx_t* ctx = (_mtx_pp_ctx_t*)arg;
    for (int i = 0; i < MTX_PING_PONG; i++) {
        xylem_mutex_lock(ctx->mtx);
        atomic_fetch_add(&ctx->value, 1);
        xylem_mutex_unlock(ctx->mtx);
    }
    int prev = atomic_fetch_add(&ctx->finished, 1);
    if (prev == 1) {
        ASSERT(atomic_load(&ctx->value) == MTX_PING_PONG * 2);
        ctx->tested = 1;
        xylem_runtime_stop();
    }
}

static void _test_mtx_pp_main(void* arg) {
    _mtx_pp_ctx_t* ctx = (_mtx_pp_ctx_t*)arg;
    _start_safety_timer();
    ctx->mtx = xylem_mutex_create();
    xylem_runtime_spawn(_mtx_ping, ctx);
    xylem_runtime_spawn(_mtx_pong, ctx);
}

static void test_mutex_ping_pong(void) {
    fprintf(stderr, "=== test_mutex_ping_pong\n");
    for (int round = 0; round < 50; round++) {
        _mtx_pp_ctx_t ctx = {0};
        xylem_runtime_start(_test_mtx_pp_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
        xylem_mutex_destroy(ctx.mtx);
    }
}

int main(void) {
    test_mutex_ping_pong();
    test_mutex_concurrent();
    return 0;
}
