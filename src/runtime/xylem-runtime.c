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

#define MINICORO_IMPL
#include "minicoro/minicoro.h"

#include "xylem/runtime/xylem-runtime.h"
#include "runtime/runtime.h"

#include "platform/platform-info.h"

#include <stdlib.h>

#define CORO_STACK_SIZE 131072 /* 128 KB */

static loop_t*     g_loop;
static thrdpool_t* g_pool;

loop_t* runtime_loop(void) {
    return g_loop;
}

thrdpool_t* runtime_pool(void) {
    return g_pool;
}

/* --- coroutine machinery --- */

typedef struct {
    void (*fn)(void*);
    void* arg;
    mco_coro* co;
} _coro_ctx_t;

static void _coro_entry(mco_coro* co) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)mco_get_user_data(co);
    ctx->fn(ctx->arg);
}

static void _coro_resume_cb(loop_t* loop, loop_post_t* req,
                            void* ud) {
    (void)loop; (void)req;
    _coro_ctx_t* ctx = (_coro_ctx_t*)ud;
    mco_coro* co = ctx->co;

    mco_resume(co);

    if (mco_status(co) == MCO_DEAD) {
        mco_destroy(co);
        free(ctx);
    }
}

void xylem_spawn(void (*fn)(void*), void* arg) {
    _coro_ctx_t* ctx = (_coro_ctx_t*)calloc(1, sizeof(_coro_ctx_t));
    if (!ctx) return;

    ctx->fn = fn;
    ctx->arg = arg;

    mco_desc desc = mco_desc_init(_coro_entry, CORO_STACK_SIZE);
    desc.user_data = ctx;

    mco_result r = mco_create(&ctx->co, &desc);
    if (r != MCO_SUCCESS) {
        free(ctx);
        return;
    }

    if (loop_is_loop_thread(g_loop)) {
        mco_resume(ctx->co);
        if (mco_status(ctx->co) == MCO_DEAD) {
            mco_destroy(ctx->co);
            free(ctx);
        }
    } else {
        loop_post(g_loop, _coro_resume_cb, ctx);
    }
}

/* --- sleep --- */

typedef struct {
    mco_coro* co;
} _sleep_ctx_t;

static void _sleep_timer_cb(loop_t* loop, loop_timer_t* timer,
                            void* ud) {
    (void)loop;
    _sleep_ctx_t* ctx = (_sleep_ctx_t*)ud;
    mco_coro* co = ctx->co;
    free(ctx);
    loop_destroy_timer(timer);
    mco_resume(co);
}

void xylem_sleep(uint64_t ms) {
    _sleep_ctx_t* ctx = (_sleep_ctx_t*)malloc(sizeof(_sleep_ctx_t));
    if (!ctx) return;

    ctx->co = mco_running();

    loop_timer_t* timer = loop_create_timer(g_loop);
    loop_start_timer(timer, _sleep_timer_cb, ctx, ms, 0);

    mco_yield(mco_running());
}

/* --- runtime lifecycle --- */

void xylem_runtime_start(void (*main_fn)(void*), void* arg,
                         xylem_runtime_opts_t* opts) {
    int32_t workers = 0;
    if (opts && opts->workers > 0)
        workers = opts->workers;
    if (workers == 0)
        workers = (int32_t)platform_info_getcpus();
    if (workers < 1)
        workers = 4;

    g_loop = loop_create();
    g_pool = thrdpool_create(workers);

    xylem_spawn(main_fn, arg);

    loop_run(g_loop);

    thrdpool_destroy(g_pool);
    loop_destroy(g_loop);
    g_pool = NULL;
    g_loop = NULL;
}

void xylem_runtime_stop(void) {
    loop_stop(g_loop);
}
