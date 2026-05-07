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

#include "xylem/sync/xylem-mutex.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "container/queue.h"
#include "sync/spin.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    queue_node_t node;
    mco_coro*    co;
} _mutex_waiter_t;

struct xylem_mutex_s {
    _Atomic uint32_t state;
    spin_t           guard;
    queue_t          waiters;
};

xylem_mutex_t* xylem_mutex_create(void) {
    xylem_mutex_t* mtx =
        (xylem_mutex_t*)calloc(1, sizeof(xylem_mutex_t));
    if (!mtx) {
        return NULL;
    }
    atomic_init(&mtx->state, 0);
    spin_init(&mtx->guard);
    queue_init(&mtx->waiters);
    return mtx;
}

void xylem_mutex_destroy(xylem_mutex_t* mtx) {
    if (!mtx) {
        return;
    }
    free(mtx);
}

typedef struct {
    xylem_mutex_t*  mtx;
    _mutex_waiter_t waiter;
} _mutex_park_ctx_t;

static bool _mutex_park_cb(mco_coro* co, void* arg) {
    _mutex_park_ctx_t* ctx = (_mutex_park_ctx_t*)arg;
    ctx->waiter.co = co;

    spin_lock(&ctx->mtx->guard);

    uint32_t expected = 0;
    if (atomic_compare_exchange_strong(&ctx->mtx->state, &expected, 1)) {
        spin_unlock(&ctx->mtx->guard);
        return false;
    }

    queue_enqueue(&ctx->mtx->waiters, &ctx->waiter.node);
    spin_unlock(&ctx->mtx->guard);
    return true;
}

void xylem_mutex_lock(xylem_mutex_t* mtx) {
    uint32_t expected = 0;
    if (atomic_compare_exchange_strong(&mtx->state, &expected, 1)) {
        return;
    }

    _mutex_park_ctx_t ctx;
    ctx.mtx = mtx;
    scheduler_park(runtime_get_scheduler(), _mutex_park_cb, &ctx);
}

void xylem_mutex_unlock(xylem_mutex_t* mtx) {
    spin_lock(&mtx->guard);
    queue_node_t* node = queue_dequeue(&mtx->waiters);
    if (!node) {
        atomic_store(&mtx->state, 0);
    }
    spin_unlock(&mtx->guard);

    if (node) {
        _mutex_waiter_t* w = queue_entry(node, _mutex_waiter_t, node);
        scheduler_schedule(runtime_get_scheduler(), w->co);
    }
}
