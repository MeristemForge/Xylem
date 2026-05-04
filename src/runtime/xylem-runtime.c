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

#include "xylem/runtime/xylem-runtime.h"

#include "runtime.h"
#include "runtime/scheduler.h"
#include "iowait.h"
#include "platform/platform-info.h"
#include "platform/platform-poller.h"

#define MINICORO_IMPL
#include "minicoro/minicoro.h"

#include <stdlib.h>

typedef struct {
    mco_coro* co;
    uint64_t  timeout_ms;
} _sleep_ctx_t;

typedef struct {
    void (*fn)(void*);
    void*        arg;
    scheduler_t* sched;
    mco_coro*    co;
} _submit_ctx_t;

static loop_t*             g_loop;
static scheduler_t*        g_sched;
static platform_poller_sq_t g_poller;
static dynpool_t*          g_dynpool;

static void _runtime_sleep_timeout_cb(
    loop_t* loop,
    loop_timer_t* timer,
    void* ud) {
    (void)loop;
    _sleep_ctx_t* ctx = (_sleep_ctx_t*)ud;
    mco_coro*     co  = ctx->co;

    loop_destroy_timer(timer);
    free(ctx);
    scheduler_schedule(g_sched, co);
}

static void _runtime_sleep_post_cb(
    loop_t* loop,
    loop_post_t* req,
    void* ud) {
    (void)loop;
    (void)req;
    _sleep_ctx_t* ctx = (_sleep_ctx_t*)ud;
    loop_timer_t* timer = loop_create_timer(g_loop);
    loop_start_timer(
        timer, _runtime_sleep_timeout_cb, ctx, ctx->timeout_ms, 0);
}

static void _runtime_submit_worker(void* arg) {
    _submit_ctx_t* ctx = (_submit_ctx_t*)arg;
    ctx->fn(ctx->arg);
    scheduler_schedule(ctx->sched, ctx->co);
    free(ctx);
}

loop_t* runtime_get_loop(void) {
    return g_loop;
}

scheduler_t* runtime_get_scheduler(void) {
    return g_sched;
}

dynpool_t* runtime_get_dynpool(void) {
    return g_dynpool;
}

platform_poller_sq_t* runtime_get_poller(void) {
    return &g_poller;
}

void xylem_runtime_spawn(void (*fn)(void*), void* arg) {
    scheduler_spawn(g_sched, fn, arg);
}

static bool _sleep_park_cb(mco_coro* co, void* arg) {
    uint64_t ms = *(uint64_t*)arg;
    _sleep_ctx_t* ctx = (_sleep_ctx_t*)malloc(sizeof(_sleep_ctx_t));
    if (!ctx) {
        return false;
    }
    ctx->co = co;
    ctx->timeout_ms = ms;
    loop_post(g_loop, _runtime_sleep_post_cb, ctx);
    return true;
}

void xylem_runtime_sleep(uint64_t ms) {
    scheduler_park(g_sched, _sleep_park_cb, &ms);
}

typedef struct {
    _submit_ctx_t* ctx;
    bool           ok;
} _submit_park_arg_t;

static bool _submit_park_cb(mco_coro* co, void* arg) {
    _submit_park_arg_t* pa = (_submit_park_arg_t*)arg;
    pa->ctx->co = co;
    pa->ok = true;
    if (dynpool_submit(g_dynpool, _runtime_submit_worker, pa->ctx) != 0) {
        pa->ok = false;
        return false;
    }
    return true;
}

int xylem_runtime_submit(void (*fn)(void*), void* arg) {
    _submit_ctx_t* ctx = (_submit_ctx_t*)malloc(sizeof(_submit_ctx_t));
    if (!ctx) {
        return -1;
    }
    ctx->fn    = fn;
    ctx->arg   = arg;
    ctx->sched = g_sched;

    _submit_park_arg_t pa = { .ctx = ctx, .ok = false };
    scheduler_park(g_sched, _submit_park_cb, &pa);

    if (!pa.ok) {
        free(ctx);
        return -1;
    }
    return 0;
}

void xylem_runtime_start(
    void (*main_fn)(void*),
    void* arg,
    xylem_runtime_opts_t* opts) {
    int32_t workers = (int32_t)platform_info_getcpus();
    if (workers < 1) {
        workers = 4;
    }
    if (opts && opts->workers > 0) {
        workers = opts->workers;
    }

    g_loop = loop_create();

    platform_poller_init(&g_poller);

    scheduler_opts_t sched_opts = { .nworkers = workers, .deque_cap = 0 };
    g_sched = scheduler_create(&sched_opts);

    scheduler_set_poller(g_sched, &g_poller, iowait_on_event);

    g_dynpool = dynpool_create(NULL);

    scheduler_spawn(g_sched, main_fn, arg);

    loop_run(g_loop);

    scheduler_destroy(g_sched);
    dynpool_destroy(g_dynpool);
    platform_poller_destroy(&g_poller);
    loop_destroy(g_loop);
}

void xylem_runtime_stop(void) {
    scheduler_shutdown(g_sched);
    loop_stop(g_loop);
}
