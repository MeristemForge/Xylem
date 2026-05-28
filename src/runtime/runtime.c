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

#include "runtime.h"

#include "platform/platform-info.h"
#include "platform/platform-sem.h"
#include "platform/platform-socket.h"

#include "platform/platform-vmem.h"

#define MINICORO_IMPL
#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    void (*fn)(void*);
    void*        arg;
    scheduler_t* sched;
    mco_coro*    co;
} _submit_ctx_t;

static scheduler_t*    g_sched;
static dynpool_t*      g_dynpool;
static platform_sem_t* g_stop_sem;
static _Atomic bool    g_shutdown;

static void _runtime_idle_cb(void* ud) {
    (void)ud;
    runtime_shutdown();
}

typedef struct {
    sched_timer_t* timer;
    uint64_t       ms;
} _sleep_park_t;

typedef struct {
    _submit_ctx_t* ctx;
    bool           ok;
} _submit_park_t;

static void _runtime_sleep_timeout_cb(sched_timer_t* timer, void* ud) {
    mco_coro* co = (mco_coro*)ud;
    sched_timer_destroy(timer);
    scheduler_schedule(g_sched, co);
}

static bool _runtime_sleep_park_fn(mco_coro* co, void* arg) {
    _sleep_park_t* p = (_sleep_park_t*)arg;
    sched_timer_start(p->timer, _runtime_sleep_timeout_cb, co, p->ms, 0);
    return true;
}

static void _runtime_submit_worker(void* arg) {
    _submit_ctx_t* ctx = (_submit_ctx_t*)arg;
    ctx->fn(ctx->arg);
    scheduler_schedule(ctx->sched, ctx->co);
    free(ctx);
}

static bool _runtime_submit_park_fn(mco_coro* co, void* arg) {
    _submit_park_t* p = (_submit_park_t*)arg;
    p->ctx->co = co;
    if (dynpool_submit(g_dynpool, _runtime_submit_worker, p->ctx) != 0) {
        p->ok = false;
        return false;
    }
    return true;
}

scheduler_t* runtime_get_scheduler(void) {
    return g_sched;
}

dynpool_t* runtime_get_dynpool(void) {
    return g_dynpool;
}

platform_poller_sq_t* runtime_get_poller(void) {
    return scheduler_get_poller(g_sched);
}

void runtime_spawn(void (*fn)(void*), void* arg) {
    scheduler_spawn(g_sched, fn, arg);
}

void runtime_sleep(uint64_t ms) {
    sched_timer_t* timer = sched_timer_create(g_sched);
    if (!timer) {
        return;
    }
    _sleep_park_t park = { .timer = timer, .ms = ms };
    scheduler_park(g_sched, _runtime_sleep_park_fn, &park);
}

int runtime_submit(void (*fn)(void*), void* arg) {
    _submit_ctx_t* ctx = (_submit_ctx_t*)malloc(sizeof(_submit_ctx_t));
    if (!ctx) {
        return -1;
    }

    ctx->fn    = fn;
    ctx->arg   = arg;
    ctx->sched = g_sched;

    _submit_park_t park = { .ctx = ctx, .ok = true };
    scheduler_park(g_sched, _runtime_submit_park_fn, &park);

    if (!park.ok) {
        free(ctx);
        return -1;
    }
    return 0;
}

void runtime_run(
    void (*main_fn)(void*),
    void* arg,
    runtime_opts_t* opts) {
    int32_t workers = (int32_t)platform_info_getcpus();
    if (workers < 1) {
        workers = 4;
    }
    if (opts && opts->workers > 0) {
        workers = opts->workers;
    }

    atomic_store(&g_shutdown, false);
    platform_socket_startup();

    scheduler_opts_t sched_opts = { .nworkers = workers, .deque_cap = 0 };
    g_sched = scheduler_create(&sched_opts);

    g_dynpool = dynpool_create(NULL);
    g_stop_sem = platform_sem_create(0);

    scheduler_set_idle_cb(g_sched, _runtime_idle_cb, NULL);
    scheduler_spawn(g_sched, main_fn, arg);

    platform_sem_wait(g_stop_sem);

    /* stop → dynpool_destroy → destroy: order prevents UAF in both directions. */
    scheduler_stop(g_sched);
    dynpool_destroy(g_dynpool);
    scheduler_destroy(g_sched);
    platform_sem_destroy(g_stop_sem);
    platform_socket_cleanup();
}

void runtime_shutdown(void) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&g_shutdown, &expected, true)) {
        platform_sem_post(g_stop_sem);
    }
}
