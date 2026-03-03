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

static void test_init(void) {
    xylem_xqueue_t queue;
    xylem_xqueue_init(&queue);
    ASSERT(xylem_xqueue_empty(&queue));
    ASSERT(xylem_xqueue_len(&queue) == 0);
    ASSERT(xylem_xqueue_front(&queue) == NULL);
}

static void test_enqueue_dequeue(void) {
    xylem_xqueue_t queue;
    xylem_xqueue_init(&queue);

    int a = 10, b = 20, c = 30;
    ASSERT(xylem_xqueue_enqueue(&queue, &a) == 0);
    ASSERT(xylem_xqueue_enqueue(&queue, &b) == 0);
    ASSERT(xylem_xqueue_enqueue(&queue, &c) == 0);

    /* FIFO: front should be 10 */
    ASSERT(*(int*)xylem_xqueue_front(&queue) == 10);
    xylem_xqueue_dequeue(&queue);

    ASSERT(*(int*)xylem_xqueue_front(&queue) == 20);
    xylem_xqueue_dequeue(&queue);

    ASSERT(*(int*)xylem_xqueue_front(&queue) == 30);
    xylem_xqueue_dequeue(&queue);

    ASSERT(xylem_xqueue_empty(&queue));
}

static void test_clear(void) {
    xylem_xqueue_t queue;
    xylem_xqueue_init(&queue);

    int vals[50];
    for (int i = 0; i < 50; i++) {
        vals[i] = i;
        xylem_xqueue_enqueue(&queue, &vals[i]);
    }
    ASSERT(xylem_xqueue_len(&queue) == 50);

    xylem_xqueue_clear(&queue);
    ASSERT(xylem_xqueue_empty(&queue));
}

int main(void) {
    test_init();
    test_enqueue_dequeue();
    test_clear();
    return 0;
}
