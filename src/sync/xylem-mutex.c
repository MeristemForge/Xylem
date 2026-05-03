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

#include "container/queue.h"

#include "minicoro/minicoro.h"

#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    queue_node_t node;
    mco_coro*    co;
} _mutex_waiter_t;

struct xylem_mutex_s {
    bool    locked;
    queue_t waiters;
};

xylem_mutex_t* xylem_mutex_create(void) {
    xylem_mutex_t* mtx =
        (xylem_mutex_t*)calloc(1, sizeof(xylem_mutex_t));
    if (!mtx) {
        return NULL;
    }
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
    if (!mtx->locked) {
        mtx->locked = true;
        return;
    }

    _mutex_waiter_t waiter;
    waiter.co = mco_running();
    queue_enqueue(&mtx->waiters, &waiter.node);
    mco_yield(mco_running());
}

void xylem_mutex_unlock(xylem_mutex_t* mtx) {
    queue_node_t* node = queue_dequeue(&mtx->waiters);
    if (node) {
        _mutex_waiter_t* w = queue_entry(node, _mutex_waiter_t, node);
        mco_resume(w->co);
    } else {
        mtx->locked = false;
    }
}
