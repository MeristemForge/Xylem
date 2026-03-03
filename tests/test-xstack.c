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
    xylem_xstack_t stack;
    xylem_xstack_init(&stack);
    ASSERT(xylem_xstack_empty(&stack));
    ASSERT(xylem_xstack_len(&stack) == 0);
    ASSERT(xylem_xstack_peek(&stack) == NULL);
}

static void test_push_pop(void) {
    xylem_xstack_t stack;
    xylem_xstack_init(&stack);

    int a = 10, b = 20, c = 30;
    ASSERT(xylem_xstack_push(&stack, &a) == 0);
    ASSERT(xylem_xstack_push(&stack, &b) == 0);
    ASSERT(xylem_xstack_push(&stack, &c) == 0);

    ASSERT(xylem_xstack_len(&stack) == 3);
    ASSERT(*(int*)xylem_xstack_peek(&stack) == 30);

    xylem_xstack_pop(&stack);
    ASSERT(*(int*)xylem_xstack_peek(&stack) == 20);

    xylem_xstack_pop(&stack);
    ASSERT(*(int*)xylem_xstack_peek(&stack) == 10);

    xylem_xstack_pop(&stack);
    ASSERT(xylem_xstack_empty(&stack));
}

static void test_clear(void) {
    xylem_xstack_t stack;
    xylem_xstack_init(&stack);

    double vals[100];
    for (int i = 0; i < 100; i++) {
        vals[i] = (double)i;
        xylem_xstack_push(&stack, &vals[i]);
    }
    ASSERT(xylem_xstack_len(&stack) == 100);

    xylem_xstack_clear(&stack);
    ASSERT(xylem_xstack_empty(&stack));
}

int main(void) {
    test_init();
    test_push_pop();
    test_clear();
    return 0;
}
