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

#include "container/mpsc.h"
#include "assert.h"
#include "xylem/xylem-threads.h"

#include <stdatomic.h>

typedef struct {
    mpsc_node_t node;
    int         value;
} _mpsc_test_node_t;

typedef struct {
    mpsc_node_t* prev;
    mpsc_node_t* node;
    atomic_bool  start;
} _mpsc_link_ctx_t;

static int _link_after_pop_starts(void* arg) {
    _mpsc_link_ctx_t* ctx = (_mpsc_link_ctx_t*)arg;
    while (!atomic_load_explicit(&ctx->start, memory_order_acquire)) {
        thrd_yield();
    }
    for (int i = 0; i < 1000; i++) {
        thrd_yield();
    }
    atomic_store_explicit(&ctx->prev->next, ctx->node, memory_order_release);
    return 0;
}

static void test_pop_waits_for_unlinked_push(void) {
    mpsc_t q;
    mpsc_init(&q);

    _mpsc_test_node_t item = { .value = 7 };
    atomic_store_explicit(&item.node.next, NULL, memory_order_relaxed);

    mpsc_node_t* prev = atomic_exchange_explicit(
        &q.tail, &item.node, memory_order_acq_rel);

    _mpsc_link_ctx_t ctx = {
        .prev = prev,
        .node = &item.node,
    };
    atomic_init(&ctx.start, false);

    thrd_t thr;
    ASSERT(thrd_create(&thr, _link_after_pop_starts, &ctx) == thrd_success);

    atomic_store_explicit(&ctx.start, true, memory_order_release);
    mpsc_node_t* popped = mpsc_pop(&q);
    ASSERT(thrd_join(thr, NULL) == thrd_success);
    ASSERT(popped == &item.node);
    ASSERT(mpsc_entry(popped, _mpsc_test_node_t, node)->value == 7);

    popped = mpsc_pop(&q);
    ASSERT(popped == NULL);
}

int main(void) {
    test_pop_waits_for_unlinked_push();
    return 0;
}
