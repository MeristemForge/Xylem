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

#include "runtime/runq.h"
#include "assert.h"
#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#define TEST_RUNQ_PRODUCERS 4
#define TEST_RUNQ_CONSUMERS 4
#define TEST_RUNQ_ITEMS_PER_PRODUCER 5000
#define TEST_RUNQ_ITEMS \
    (TEST_RUNQ_PRODUCERS * TEST_RUNQ_ITEMS_PER_PRODUCER)
#define TEST_RUNQ_BATCH_CAP 32

typedef struct {
    runq_node_t node;
    int         value;
    atomic_int  seen;
} _runq_test_item_t;

typedef struct {
    runq_t*            runq;
    _runq_test_item_t* items;
    atomic_bool        start;
    atomic_int         producers_done;
    atomic_int         consumed_count;
} _runq_test_ctx_t;

typedef struct {
    _runq_test_ctx_t* ctx;
    int               producer_index;
} _runq_producer_arg_t;

static int _runq_producer_thread(void* arg) {
    _runq_producer_arg_t* producer_arg = (_runq_producer_arg_t*)arg;
    _runq_test_ctx_t* ctx = producer_arg->ctx;
    int first_index =
        producer_arg->producer_index * TEST_RUNQ_ITEMS_PER_PRODUCER;
    int end_index = first_index + TEST_RUNQ_ITEMS_PER_PRODUCER;

    while (!atomic_load(&ctx->start)) {
        thrd_yield();
    }
    for (int i = first_index; i < end_index; i++) {
        runq_push(ctx->runq, &ctx->items[i].node);
    }
    atomic_fetch_add(&ctx->producers_done, 1);
    return 0;
}

static int _runq_consumer_thread(void* arg) {
    _runq_test_ctx_t* ctx = (_runq_test_ctx_t*)arg;
    runq_node_t* nodes[TEST_RUNQ_BATCH_CAP];

    while (!atomic_load(&ctx->start)) {
        thrd_yield();
    }
    for (;;) {
        int count = runq_pop_fair(
            ctx->runq,
            nodes,
            TEST_RUNQ_BATCH_CAP,
            TEST_RUNQ_CONSUMERS);
        for (int i = 0; i < count; i++) {
            _runq_test_item_t* item =
                runq_entry(nodes[i], _runq_test_item_t, node);
            ASSERT(atomic_fetch_add(&item->seen, 1) == 0);
        }
        if (count > 0) {
            atomic_fetch_add(&ctx->consumed_count, count);
            continue;
        }
        if (atomic_load(&ctx->producers_done) == TEST_RUNQ_PRODUCERS
            && atomic_load(&ctx->consumed_count) == TEST_RUNQ_ITEMS) {
            return 0;
        }
        thrd_yield();
    }
}

static void test_node_is_singly_linked(void) {
    ASSERT(sizeof(runq_node_t) == sizeof(void*));
}

static void test_fifo(void) {
    _runq_test_item_t items[3] = {
        {.value = 1},
        {.value = 2},
        {.value = 3},
    };
    runq_t* runq = runq_create();
    ASSERT(runq != NULL);

    for (int i = 0; i < 3; i++) {
        runq_push(runq, &items[i].node);
    }
    for (int i = 0; i < 3; i++) {
        runq_node_t* node = runq_pop(runq);
        ASSERT(runq_entry(node, _runq_test_item_t, node)->value == i + 1);
    }
    ASSERT(runq_pop(runq) == NULL);
    runq_destroy(runq);
}

static void test_destroy_accepts_null(void) {
    runq_destroy(NULL);
}

static void test_push_batch_ignores_empty_batch(void) {
    runq_t* runq = runq_create();
    ASSERT(runq != NULL);

    runq_push_batch(runq, NULL, 0);
    ASSERT(runq_pop(runq) == NULL);
    runq_destroy(runq);
}

static void test_push_batch_preserves_order(void) {
    _runq_test_item_t items[5] = {
        {.value = 1},
        {.value = 2},
        {.value = 3},
        {.value = 4},
        {.value = 5},
    };
    runq_node_t* nodes[5];
    runq_t* runq = runq_create();
    ASSERT(runq != NULL);

    for (int i = 0; i < 5; i++) {
        nodes[i] = &items[i].node;
    }
    runq_push_batch(runq, nodes, 5);
    for (int i = 0; i < 5; i++) {
        ASSERT(runq_pop(runq) == &items[i].node);
    }
    runq_destroy(runq);
}

static void test_pop_fair_rejects_invalid_limits(void) {
    runq_node_t* nodes[1];
    runq_t* runq = runq_create();
    ASSERT(runq != NULL);

    ASSERT(runq_pop_fair(runq, nodes, 0, 1) == 0);
    ASSERT(runq_pop_fair(runq, nodes, 1, 0) == 0);
    runq_destroy(runq);
}

static void test_pop_fair_share(void) {
    _runq_test_item_t items[10] = {0};
    runq_node_t* input_nodes[10];
    runq_node_t* output_nodes[8];
    runq_t* runq = runq_create();
    ASSERT(runq != NULL);

    for (int i = 0; i < 10; i++) {
        items[i].value = i;
        input_nodes[i] = &items[i].node;
    }
    runq_push_batch(runq, input_nodes, 10);

    ASSERT(runq_pop_fair(runq, output_nodes, 8, 3) == 4);
    for (int i = 0; i < 4; i++) {
        ASSERT(output_nodes[i] == &items[i].node);
    }
    ASSERT(runq_pop_fair(runq, output_nodes, 3, 3) == 3);
    for (int i = 0; i < 3; i++) {
        ASSERT(output_nodes[i] == &items[i + 4].node);
    }
    ASSERT(runq_pop_fair(runq, output_nodes, 8, 1) == 3);
    for (int i = 0; i < 3; i++) {
        ASSERT(output_nodes[i] == &items[i + 7].node);
    }
    ASSERT(runq_pop(runq) == NULL);
    runq_destroy(runq);
}

static void test_concurrent_producers_and_consumers(void) {
    _runq_test_item_t* items = (_runq_test_item_t*)calloc(
        TEST_RUNQ_ITEMS, sizeof(_runq_test_item_t));
    ASSERT(items != NULL);

    runq_t* runq = runq_create();
    ASSERT(runq != NULL);

    _runq_test_ctx_t ctx = {
        .runq = runq,
        .items = items,
    };
    atomic_init(&ctx.start, false);
    atomic_init(&ctx.producers_done, 0);
    atomic_init(&ctx.consumed_count, 0);
    for (int i = 0; i < TEST_RUNQ_ITEMS; i++) {
        items[i].value = i;
        atomic_init(&items[i].seen, 0);
    }

    thrd_t producers[TEST_RUNQ_PRODUCERS];
    thrd_t consumers[TEST_RUNQ_CONSUMERS];
    _runq_producer_arg_t producer_args[TEST_RUNQ_PRODUCERS];
    for (int i = 0; i < TEST_RUNQ_PRODUCERS; i++) {
        producer_args[i].ctx = &ctx;
        producer_args[i].producer_index = i;
        ASSERT(thrd_create(
                   &producers[i],
                   _runq_producer_thread,
                   &producer_args[i])
               == thrd_success);
    }
    for (int i = 0; i < TEST_RUNQ_CONSUMERS; i++) {
        ASSERT(thrd_create(&consumers[i], _runq_consumer_thread, &ctx)
               == thrd_success);
    }

    atomic_store(&ctx.start, true);
    for (int i = 0; i < TEST_RUNQ_PRODUCERS; i++) {
        ASSERT(thrd_join(producers[i], NULL) == thrd_success);
    }
    for (int i = 0; i < TEST_RUNQ_CONSUMERS; i++) {
        ASSERT(thrd_join(consumers[i], NULL) == thrd_success);
    }

    ASSERT(atomic_load(&ctx.consumed_count) == TEST_RUNQ_ITEMS);
    for (int i = 0; i < TEST_RUNQ_ITEMS; i++) {
        ASSERT(atomic_load(&items[i].seen) == 1);
    }
    ASSERT(runq_pop(runq) == NULL);

    runq_destroy(runq);
    free(items);
}

int main(void) {
    test_node_is_singly_linked();
    test_fifo();
    test_destroy_accepts_null();
    test_push_batch_ignores_empty_batch();
    test_push_batch_preserves_order();
    test_pop_fair_rejects_invalid_limits();
    test_pop_fair_share();
    test_concurrent_producers_and_consumers();
    return 0;
}
