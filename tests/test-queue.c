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

#include "xylem.h"
#include "assert.h"

typedef struct test_item_s {
    int                 value;
    xylem_queue_node_t  node;
} test_item_t;

static void test_queue_init(void) {
    xylem_queue_t queue;
    xylem_queue_init(&queue);
    ASSERT(xylem_queue_empty(&queue) == true);
    ASSERT(xylem_queue_len(&queue) == 0);
    ASSERT(xylem_queue_front(&queue) == NULL);
    ASSERT(xylem_queue_back(&queue) == NULL);
}

static void test_queue_enqueue_dequeue(void) {
    xylem_queue_t queue;
    test_item_t   a = {.value = 1};
    test_item_t   b = {.value = 2};
    test_item_t   c = {.value = 3};

    xylem_queue_init(&queue);
    xylem_queue_enqueue(&queue, &a.node);
    xylem_queue_enqueue(&queue, &b.node);
    xylem_queue_enqueue(&queue, &c.node);
    ASSERT(xylem_queue_len(&queue) == 3);

    /* FIFO order: a, b, c */
    xylem_queue_node_t* n = xylem_queue_dequeue(&queue);
    ASSERT(n == &a.node);
    ASSERT(xylem_queue_len(&queue) == 2);

    n = xylem_queue_dequeue(&queue);
    ASSERT(n == &b.node);

    n = xylem_queue_dequeue(&queue);
    ASSERT(n == &c.node);
    ASSERT(xylem_queue_empty(&queue) == true);

    /* Dequeue from empty queue */
    n = xylem_queue_dequeue(&queue);
    ASSERT(n == NULL);
}

static void test_queue_front_back(void) {
    xylem_queue_t queue;
    test_item_t   a = {.value = 10};
    test_item_t   b = {.value = 20};

    xylem_queue_init(&queue);
    xylem_queue_enqueue(&queue, &a.node);
    ASSERT(xylem_queue_front(&queue) == &a.node);
    ASSERT(xylem_queue_back(&queue) == &a.node);

    xylem_queue_enqueue(&queue, &b.node);
    ASSERT(xylem_queue_front(&queue) == &a.node);
    ASSERT(xylem_queue_back(&queue) == &b.node);
}

static void test_queue_entry(void) {
    xylem_queue_t queue;
    test_item_t   item = {.value = 42};

    xylem_queue_init(&queue);
    xylem_queue_enqueue(&queue, &item.node);

    xylem_queue_node_t* n = xylem_queue_front(&queue);
    test_item_t*        recovered = xylem_queue_entry(n, test_item_t, node);
    ASSERT(recovered->value == 42);
}

static void test_queue_swap(void) {
    xylem_queue_t q1, q2;
    test_item_t   a = {.value = 1};
    test_item_t   b = {.value = 2};
    test_item_t   c = {.value = 3};

    xylem_queue_init(&q1);
    xylem_queue_init(&q2);

    /* Swap two empty queues */
    xylem_queue_swap(&q1, &q2);
    ASSERT(xylem_queue_empty(&q1));
    ASSERT(xylem_queue_empty(&q2));

    /* Swap non-empty with empty */
    xylem_queue_enqueue(&q1, &a.node);
    xylem_queue_enqueue(&q1, &b.node);
    xylem_queue_swap(&q1, &q2);
    ASSERT(xylem_queue_empty(&q1));
    ASSERT(xylem_queue_len(&q2) == 2);
    ASSERT(xylem_queue_front(&q2) == &a.node);
    ASSERT(xylem_queue_back(&q2) == &b.node);

    /* Swap non-empty with non-empty */
    xylem_queue_enqueue(&q1, &c.node);
    xylem_queue_swap(&q1, &q2);
    ASSERT(xylem_queue_len(&q1) == 2);
    ASSERT(xylem_queue_len(&q2) == 1);
    ASSERT(xylem_queue_front(&q1) == &a.node);
    ASSERT(xylem_queue_front(&q2) == &c.node);

    /* Drain q1 to verify FIFO order preserved after swap */
    xylem_queue_node_t* n = xylem_queue_dequeue(&q1);
    ASSERT(n == &a.node);
    n = xylem_queue_dequeue(&q1);
    ASSERT(n == &b.node);
    ASSERT(xylem_queue_empty(&q1));
}

int main(void) {
    test_queue_init();
    test_queue_enqueue_dequeue();
    test_queue_front_back();
    test_queue_entry();
    test_queue_swap();
    return 0;
}
