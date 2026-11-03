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

#include "xylem/xylem-threads.h"
#include "assert.h"

#include "container/mpsc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#define PRODUCER_COUNT 4
#define ITEMS_PER_PRODUCER 5000
#define ITEM_COUNT (PRODUCER_COUNT * ITEMS_PER_PRODUCER)

typedef struct {
    mpsc_node_t node;
    uint32_t    producer;
    uint32_t    sequence;
} _item_t;

typedef struct {
    mpsc_t*     queue;
    _item_t*    items;
    atomic_bool start;
    atomic_int  finished;
} _concurrent_ctx_t;

typedef struct {
    _concurrent_ctx_t* ctx;
    uint32_t          producer;
} _producer_ctx_t;

static int _producer_thread(void* arg) {
    _producer_ctx_t* producer = (_producer_ctx_t*)arg;
    _concurrent_ctx_t* ctx = producer->ctx;
    size_t first = (size_t)producer->producer * ITEMS_PER_PRODUCER;
    while (!atomic_load(&ctx->start)) {
        thrd_yield();
    }
    for (size_t i = 0; i < ITEMS_PER_PRODUCER; i++) {
        mpsc_push(ctx->queue, &ctx->items[first + i].node);
    }
    atomic_fetch_add(&ctx->finished, 1);
    return 0;
}

static void test_fifo(void) {
    mpsc_t queue;
    mpsc_init(&queue);
    ASSERT(!mpsc_can_pop(&queue));
    ASSERT(mpsc_pop(&queue) == NULL);
    _item_t items[3] = {
        {.sequence = 0},
        {.sequence = 1},
        {.sequence = 2},
    };
    for (size_t i = 0; i < 3; i++) {
        mpsc_push(&queue, &items[i].node);
    }
    for (size_t i = 0; i < 3; i++) {
        ASSERT(mpsc_can_pop(&queue));
        mpsc_node_t* node = mpsc_pop(&queue);
        ASSERT(node == &items[i].node);
        ASSERT(mpsc_entry(node, _item_t, node)->sequence == i);
    }
    ASSERT(!mpsc_can_pop(&queue));
    ASSERT(mpsc_pop(&queue) == NULL);
}

static void test_node_reuse(void) {
    mpsc_t queue;
    mpsc_init(&queue);
    _item_t item = {0};
    for (uint32_t i = 0; i < 200; i++) {
        item.sequence = i;
        mpsc_push(&queue, &item.node);
        mpsc_node_t* node = mpsc_pop(&queue);
        ASSERT(node == &item.node);
        ASSERT(mpsc_entry(node, _item_t, node)->sequence == i);
        ASSERT(mpsc_pop(&queue) == NULL);
    }
}

static void test_concurrent_producers(void) {
    mpsc_t queue;
    mpsc_init(&queue);
    _item_t* items = (_item_t*)calloc(ITEM_COUNT, sizeof(_item_t));
    ASSERT(items != NULL);
    for (size_t i = 0; i < ITEM_COUNT; i++) {
        items[i].producer = (uint32_t)(i / ITEMS_PER_PRODUCER);
        items[i].sequence = (uint32_t)(i % ITEMS_PER_PRODUCER);
    }
    _concurrent_ctx_t ctx = {.queue = &queue, .items = items};
    thrd_t threads[PRODUCER_COUNT];
    _producer_ctx_t producers[PRODUCER_COUNT];
    for (int i = 0; i < PRODUCER_COUNT; i++) {
        producers[i] = (_producer_ctx_t){.ctx = &ctx, .producer = (uint32_t)i};
        ASSERT(thrd_create(&threads[i], _producer_thread, &producers[i])
               == thrd_success);
    }
    atomic_store(&ctx.start, true);
    uint32_t next[PRODUCER_COUNT] = {0};
    for (size_t consumed = 0; consumed < ITEM_COUNT; ) {
        mpsc_node_t* node = mpsc_pop(&queue);
        if (!node && atomic_load(&ctx.finished) == PRODUCER_COUNT) {
            /* Recheck after observing every producer's final push. */
            node = mpsc_pop(&queue);
            ASSERT(node != NULL);
        }
        if (!node) {
            thrd_yield();
            continue;
        }
        _item_t* item = mpsc_entry(node, _item_t, node);
        ASSERT(item->producer < PRODUCER_COUNT);
        ASSERT(item->sequence < ITEMS_PER_PRODUCER);
        ASSERT(item->sequence == next[item->producer]);
        ASSERT(item == &items[(size_t)item->producer * ITEMS_PER_PRODUCER
                              + item->sequence]);
        next[item->producer]++;
        consumed++;
    }
    for (int i = 0; i < PRODUCER_COUNT; i++) {
        ASSERT(thrd_join(threads[i], NULL) == thrd_success);
        ASSERT(next[i] == ITEMS_PER_PRODUCER);
    }
    ASSERT(!mpsc_can_pop(&queue));
    ASSERT(mpsc_pop(&queue) == NULL);
    free(items);
}

int main(void) {
    test_fifo();
    test_node_reuse();
    test_concurrent_producers();
    return 0;
}
