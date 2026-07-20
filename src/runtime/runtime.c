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
#include "runtime/precond.h"

#define MCO_GET_PAGE_SIZE() platform_vmem_page_size()
#define MINICORO_IMPL
#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct _submit_ctx_s {
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

typedef struct _sleep_park_s {
    scheduler_timer_t* timer;
    uint64_t       ms;
} _sleep_park_t;

typedef struct _submit_park_s {
    _submit_ctx_t* ctx;
    bool           ok;
} _submit_park_t;

static void _runtime_sleep_timeout_cb(scheduler_timer_t* timer, void* ud) {
    mco_coro* co = (mco_coro*)ud;
    scheduler_timer_destroy(timer);
    scheduler_coro_ready(g_sched, co);
}

static bool _runtime_sleep_commit_cb(mco_coro* co, void* arg) {
    _sleep_park_t* p = (_sleep_park_t*)arg;
    scheduler_timer_start(
        p->timer,
        _runtime_sleep_timeout_cb,
        co,
        p->ms,
        0);
    return true;
}

static void _runtime_submit_work_cb(void* arg) {
    _submit_ctx_t* ctx = (_submit_ctx_t*)arg;
    ctx->fn(ctx->arg);
    scheduler_coro_ready(ctx->sched, ctx->co);
    free(ctx);
}

static bool _runtime_submit_commit_cb(mco_coro* co, void* arg) {
    _submit_park_t* p = (_submit_park_t*)arg;
    p->ctx->co = co;
    if (dynpool_submit(g_dynpool, _runtime_submit_work_cb, p->ctx) != 0) {
        free(p->ctx);
        p->ctx = NULL;
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

int runtime_spawn(void (*fn)(void*), void* arg) {
    int rc = scheduler_coro_spawn(g_sched, fn, arg);
    if (rc != 0) {
        return rc;
    }
    if (runtime_consume_credit(RUNTIME_CREDIT_COST)) {
        runtime_yield();
    }
    return 0;
}

void runtime_sleep(uint64_t ms) {
    RUNTIME_REQUIRE_COROUTINE("runtime", "runtime_sleep");

    scheduler_timer_t* timer = scheduler_timer_create(g_sched);
    if (!timer) {
        return;
    }
    _sleep_park_t park = { .timer = timer, .ms = ms };
    scheduler_coro_park(g_sched, _runtime_sleep_commit_cb, &park);
}

bool runtime_consume_credit(uint32_t cost) {
    return scheduler_coro_consume_credit(cost);
}

void runtime_yield(void) {
    scheduler_coro_yield();
}

int runtime_submit(void (*fn)(void*), void* arg) {
    if (!fn) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("runtime", "runtime_submit");

    _submit_ctx_t* ctx = (_submit_ctx_t*)calloc(1, sizeof(_submit_ctx_t));
    if (!ctx) {
        return -1;
    }

    ctx->fn    = fn;
    ctx->arg   = arg;
    ctx->sched = g_sched;

    _submit_park_t park = { .ctx = ctx, .ok = true };
    scheduler_coro_park(g_sched, _runtime_submit_commit_cb, &park);

    if (!park.ok) {
        return -1;
    }
    return 0;
}

void runtime_run(
    void (*main_fn)(void*),
    void* arg,
    runtime_opts_t* opts) {
    if (!main_fn) {
        return;
    }

    int32_t workers = (int32_t)platform_info_getcpus();
    if (workers < 1) {
        workers = 4;
    }
    if (opts && opts->workers > 0) {
        workers = opts->workers;
    }

    atomic_store(&g_shutdown, false);
    platform_socket_startup();

    scheduler_opts_t sched_opts = {.worker_count = workers};
    if (opts && opts->coro_stack_size > 0) {
        sched_opts.coro_stack_size = opts->coro_stack_size;
    }
    g_sched = scheduler_create(&sched_opts);
    if (!g_sched) {
        platform_socket_cleanup();
        return;
    }

    g_dynpool = dynpool_create(NULL);
    if (!g_dynpool) {
        scheduler_destroy(g_sched);
        g_sched = NULL;
        platform_socket_cleanup();
        return;
    }

    g_stop_sem = platform_sem_create(0);
    if (!g_stop_sem) {
        dynpool_destroy(g_dynpool);
        g_dynpool = NULL;
        scheduler_destroy(g_sched);
        g_sched = NULL;
        platform_socket_cleanup();
        return;
    }

    scheduler_set_idle_cb(g_sched, _runtime_idle_cb, NULL);
    if (scheduler_coro_spawn(g_sched, main_fn, arg) != 0) {
        scheduler_stop(g_sched);
        dynpool_destroy(g_dynpool);
        scheduler_destroy(g_sched);
        platform_sem_destroy(g_stop_sem);
        g_dynpool  = NULL;
        g_sched    = NULL;
        g_stop_sem = NULL;
        platform_socket_cleanup();
        return;
    }

    platform_sem_wait(g_stop_sem);

    /* stop -> dynpool_destroy -> destroy: order prevents UAF. */
    scheduler_stop(g_sched);
    dynpool_destroy(g_dynpool);
    scheduler_destroy(g_sched);
    platform_sem_destroy(g_stop_sem);
    g_dynpool  = NULL;
    g_sched    = NULL;
    g_stop_sem = NULL;
    platform_socket_cleanup();
}

void runtime_shutdown(void) {
    platform_sem_t* stop_sem = g_stop_sem;
    if (!stop_sem) {
        return;
    }

    bool expected = false;
    if (atomic_compare_exchange_strong(&g_shutdown, &expected, true)) {
        platform_sem_post(stop_sem);
    }
}
