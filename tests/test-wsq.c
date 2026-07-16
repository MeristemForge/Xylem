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

#include "runtime/wsq.h"
#include "assert.h"
#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#define TEST_WSQ_CAP 64
#define TEST_WSQ_CONSUMERS 4
#define TEST_WSQ_ITEMS 20000

typedef struct {
    atomic_int seen;
} _wsq_test_item_t;

typedef struct {
    wsq_t*      q;
    atomic_bool producer_done;
    atomic_int  consumed;
} _wsq_test_ctx_t;

static void _record_consumed(_wsq_test_ctx_t* ctx, void* elem) {
    _wsq_test_item_t* item = (_wsq_test_item_t*)elem;
    ASSERT(atomic_fetch_add(&item->seen, 1) == 0);
    atomic_fetch_add(&ctx->consumed, 1);
}

static int _consumer_thread(void* arg) {
    _wsq_test_ctx_t* ctx = (_wsq_test_ctx_t*)arg;
    void* elems[TEST_WSQ_CAP / 2];

    for (;;) {
        int n = wsq_steal_half(ctx->q, elems, TEST_WSQ_CAP / 2);
        for (int i = 0; i < n; i++) {
            _record_consumed(ctx, elems[i]);
        }
        if (n > 0) {
            continue;
        }
        if (atomic_load(&ctx->producer_done)
            && atomic_load(&ctx->consumed) == TEST_WSQ_ITEMS) {
            return 0;
        }
        thrd_yield();
    }
}

static void test_create_rejects_invalid_capacity(void) {
    ASSERT(wsq_create(0) == NULL);
    ASSERT(wsq_create(-1) == NULL);
    ASSERT(wsq_create(3) == NULL);
}

static void test_push_rejects_null(void) {
    wsq_t* q = wsq_create(2);
    ASSERT(q != NULL);
    ASSERT(wsq_push(q, NULL) == -1);
    wsq_destroy(q);
}

static void test_fifo_and_remaining(void) {
    int a = 1;
    int b = 2;
    int c = 3;
    int d = 4;
    int e = 5;

    wsq_t* q = wsq_create(4);
    ASSERT(q != NULL);
    ASSERT(wsq_remaining(q) == 4);
    ASSERT(wsq_push(q, &a) == 0);
    ASSERT(wsq_push(q, &b) == 0);
    ASSERT(wsq_push(q, &c) == 0);
    ASSERT(wsq_push(q, &d) == 0);
    ASSERT(wsq_remaining(q) == 0);
    ASSERT(wsq_push(q, &e) == -1);
    ASSERT(wsq_pop(q) == &a);
    ASSERT(wsq_pop(q) == &b);
    ASSERT(wsq_pop(q) == &c);
    ASSERT(wsq_pop(q) == &d);
    ASSERT(wsq_pop(q) == NULL);
    ASSERT(wsq_remaining(q) == 4);
    wsq_destroy(q);
}

static void test_wraparound(void) {
    int items[8];
    wsq_t* q = wsq_create(8);
    ASSERT(q != NULL);

    for (int round = 0; round < 1000; round++) {
        for (int i = 0; i < 8; i++) {
            items[i] = round * 8 + i;
            ASSERT(wsq_push(q, &items[i]) == 0);
        }
        for (int i = 0; i < 8; i++) {
            ASSERT(wsq_pop(q) == &items[i]);
        }
    }

    wsq_destroy(q);
}

static void test_pop_half(void) {
    int items[8];
    void* elems[4];
    wsq_t* q = wsq_create(8);
    ASSERT(q != NULL);

    for (int i = 0; i < 8; i++) {
        items[i] = i;
        ASSERT(wsq_push(q, &items[i]) == 0);
    }

    ASSERT(wsq_pop_half(q, elems, 4) == 4);
    for (int i = 0; i < 4; i++) {
        ASSERT(elems[i] == &items[i]);
    }
    ASSERT(wsq_pop(q) == &items[4]);
    wsq_destroy(q);
}

static void test_steal_half_odd(void) {
    int items[5];
    void* elems[3];
    wsq_t* q = wsq_create(8);
    ASSERT(q != NULL);

    for (int i = 0; i < 5; i++) {
        items[i] = i;
        ASSERT(wsq_push(q, &items[i]) == 0);
    }

    ASSERT(wsq_steal_half(q, elems, 0) == 0);
    ASSERT(wsq_steal_half(q, elems, 3) == 3);
    for (int i = 0; i < 3; i++) {
        ASSERT(elems[i] == &items[i]);
    }
    ASSERT(wsq_pop(q) == &items[3]);
    wsq_destroy(q);
}

static void test_concurrent_owner_and_thieves(void) {
    _wsq_test_item_t* items = (_wsq_test_item_t*)calloc(
        TEST_WSQ_ITEMS, sizeof(_wsq_test_item_t));
    ASSERT(items != NULL);

    wsq_t* q = wsq_create(TEST_WSQ_CAP);
    ASSERT(q != NULL);

    _wsq_test_ctx_t ctx = {
        .q = q,
    };
    atomic_init(&ctx.producer_done, false);
    atomic_init(&ctx.consumed, 0);
    for (int i = 0; i < TEST_WSQ_ITEMS; i++) {
        atomic_init(&items[i].seen, 0);
    }

    thrd_t consumers[TEST_WSQ_CONSUMERS];
    for (int i = 0; i < TEST_WSQ_CONSUMERS; i++) {
        ASSERT(thrd_create(&consumers[i], _consumer_thread, &ctx)
               == thrd_success);
    }

    void* owner_elems[TEST_WSQ_CAP / 2];
    for (int i = 0; i < TEST_WSQ_ITEMS; i++) {
        while (wsq_push(q, &items[i]) != 0) {
            void* elem = wsq_pop(q);
            if (elem) {
                _record_consumed(&ctx, elem);
            } else {
                thrd_yield();
            }
        }
        if ((i + 1) % TEST_WSQ_CAP == 0) {
            int n = wsq_pop_half(q, owner_elems, TEST_WSQ_CAP / 2);
            for (int j = 0; j < n; j++) {
                _record_consumed(&ctx, owner_elems[j]);
            }
        }
    }
    atomic_store(&ctx.producer_done, true);

    while (atomic_load(&ctx.consumed) < TEST_WSQ_ITEMS) {
        int n = wsq_pop_half(q, owner_elems, TEST_WSQ_CAP / 2);
        for (int i = 0; i < n; i++) {
            _record_consumed(&ctx, owner_elems[i]);
        }
        if (n == 0) {
            thrd_yield();
        }
    }

    for (int i = 0; i < TEST_WSQ_CONSUMERS; i++) {
        ASSERT(thrd_join(consumers[i], NULL) == thrd_success);
    }
    ASSERT(atomic_load(&ctx.consumed) == TEST_WSQ_ITEMS);
    for (int i = 0; i < TEST_WSQ_ITEMS; i++) {
        ASSERT(atomic_load(&items[i].seen) == 1);
    }

    wsq_destroy(q);
    free(items);
}

int main(void) {
    test_create_rejects_invalid_capacity();
    test_push_rejects_null();
    test_fifo_and_remaining();
    test_wraparound();
    test_pop_half();
    test_steal_half_odd();
    test_concurrent_owner_and_thieves();
    return 0;
}
