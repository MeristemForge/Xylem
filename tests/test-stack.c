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
    int32_t             value;
    xylem_stack_node_t  node;
} test_item_t;

static void test_stack_init(void) {
    xylem_stack_t stack;
    xylem_stack_init(&stack);
    ASSERT(xylem_stack_empty(&stack) == true);
    ASSERT(xylem_stack_len(&stack) == 0);
    ASSERT(xylem_stack_peek(&stack) == NULL);
}

static void test_stack_push_pop(void) {
    xylem_stack_t stack;
    test_item_t   a = {.value = 1};
    test_item_t   b = {.value = 2};
    test_item_t   c = {.value = 3};

    xylem_stack_init(&stack);
    xylem_stack_push(&stack, &a.node);
    xylem_stack_push(&stack, &b.node);
    xylem_stack_push(&stack, &c.node);
    ASSERT(xylem_stack_len(&stack) == 3);

    /* LIFO order: c, b, a */
    xylem_stack_node_t* n = xylem_stack_pop(&stack);
    ASSERT(n == &c.node);
    ASSERT(xylem_stack_len(&stack) == 2);

    n = xylem_stack_pop(&stack);
    ASSERT(n == &b.node);

    n = xylem_stack_pop(&stack);
    ASSERT(n == &a.node);
    ASSERT(xylem_stack_empty(&stack) == true);

    /* Pop from empty stack */
    n = xylem_stack_pop(&stack);
    ASSERT(n == NULL);
}

static void test_stack_peek(void) {
    xylem_stack_t stack;
    test_item_t   a = {.value = 10};
    test_item_t   b = {.value = 20};

    xylem_stack_init(&stack);
    xylem_stack_push(&stack, &a.node);
    ASSERT(xylem_stack_peek(&stack) == &a.node);

    xylem_stack_push(&stack, &b.node);
    ASSERT(xylem_stack_peek(&stack) == &b.node);

    /* Peek doesn't remove */
    ASSERT(xylem_stack_len(&stack) == 2);
}

static void test_stack_entry(void) {
    xylem_stack_t stack;
    test_item_t   item = {.value = 42};

    xylem_stack_init(&stack);
    xylem_stack_push(&stack, &item.node);

    xylem_stack_node_t* n = xylem_stack_peek(&stack);
    test_item_t*        recovered = xylem_stack_entry(n, test_item_t, node);
    ASSERT(recovered->value == 42);
}

int main(void) {
    test_stack_init();
    test_stack_push_pop();
    test_stack_peek();
    test_stack_entry();
    return 0;
}
