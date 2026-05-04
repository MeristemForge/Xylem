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
#include "container/queue.h"

#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    queue_node_t node;
    mco_coro*    co;
} _mutex_waiter_t;

struct xylem_mutex_s {
    _Atomic uint32_t state;
    atomic_flag      guard;
    queue_t          waiters;
};

static void _spin_lock(atomic_flag* flag) {
    while (atomic_flag_test_and_set_explicit(flag, memory_order_acquire)) {
        /* spin */
    }
}

static void _spin_unlock(atomic_flag* flag) {
    atomic_flag_clear_explicit(flag, memory_order_release);
}

xylem_mutex_t* xylem_mutex_create(void) {
    xylem_mutex_t* mtx =
        (xylem_mutex_t*)calloc(1, sizeof(xylem_mutex_t));
    if (!mtx) {
        return NULL;
    }
    atomic_init(&mtx->state, 0);
    atomic_flag_clear(&mtx->guard);
    queue_init(&mtx->waiters);
    return mtx;
}

void xylem_mutex_destroy(xylem_mutex_t* mtx) {
    if (!mtx) {
        return;
    }
    free(mtx);
}

void xylem_mutex_lock(xylem_mutex_t* mtx) {
    uint32_t expected = 0;
    if (atomic_compare_exchange_strong(&mtx->state, &expected, 1)) {
        return;
    }

    _mutex_waiter_t waiter;
    waiter.co = mco_running();

    _spin_lock(&mtx->guard);
    queue_enqueue(&mtx->waiters, &waiter.node);
    _spin_unlock(&mtx->guard);

    mco_yield(mco_running());
}

void xylem_mutex_unlock(xylem_mutex_t* mtx) {
    _spin_lock(&mtx->guard);
    queue_node_t* node = queue_dequeue(&mtx->waiters);
    _spin_unlock(&mtx->guard);

    if (node) {
        _mutex_waiter_t* w = queue_entry(node, _mutex_waiter_t, node);
        scheduler_schedule(runtime_get_scheduler(), w->co);
    } else {
        atomic_store(&mtx->state, 0);
    }
}
