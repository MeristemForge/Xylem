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
    int                value;
    xylem_list_node_t  node;
} test_item_t;

static void test_list_init(void) {
    xylem_list_t list;
    xylem_list_init(&list);
    ASSERT(xylem_list_empty(&list) == true);
    ASSERT(xylem_list_len(&list) == 0);
    ASSERT(xylem_list_front(&list) == NULL);
    ASSERT(xylem_list_back(&list) == NULL);
}

static void test_list_push_front(void) {
    xylem_list_t list;
    test_item_t  a = {.value = 1};
    test_item_t  b = {.value = 2};

    xylem_list_init(&list);
    xylem_list_push_front(&list, &a.node);
    ASSERT(xylem_list_len(&list) == 1);
    ASSERT(xylem_list_front(&list) == &a.node);
    ASSERT(xylem_list_back(&list) == &a.node);

    xylem_list_push_front(&list, &b.node);
    ASSERT(xylem_list_len(&list) == 2);
    ASSERT(xylem_list_front(&list) == &b.node);
    ASSERT(xylem_list_back(&list) == &a.node);
}

static void test_list_push_back(void) {
    xylem_list_t list;
    test_item_t  a = {.value = 1};
    test_item_t  b = {.value = 2};

    xylem_list_init(&list);
    xylem_list_push_back(&list, &a.node);
    ASSERT(xylem_list_len(&list) == 1);

    xylem_list_push_back(&list, &b.node);
    ASSERT(xylem_list_len(&list) == 2);
    ASSERT(xylem_list_front(&list) == &a.node);
    ASSERT(xylem_list_back(&list) == &b.node);
}

static void test_list_remove(void) {
    xylem_list_t list;
    test_item_t  a = {.value = 1};
    test_item_t  b = {.value = 2};
    test_item_t  c = {.value = 3};

    xylem_list_init(&list);
    xylem_list_push_back(&list, &a.node);
    xylem_list_push_back(&list, &b.node);
    xylem_list_push_back(&list, &c.node);

    /* Remove middle node */
    xylem_list_remove(&list, &b.node);
    ASSERT(xylem_list_len(&list) == 2);
    ASSERT(xylem_list_front(&list) == &a.node);
    ASSERT(xylem_list_back(&list) == &c.node);

    /* Remove front node */
    xylem_list_remove(&list, &a.node);
    ASSERT(xylem_list_len(&list) == 1);
    ASSERT(xylem_list_front(&list) == &c.node);

    /* Remove last node */
    xylem_list_remove(&list, &c.node);
    ASSERT(xylem_list_empty(&list) == true);
}

static void test_list_entry(void) {
    xylem_list_t list;
    test_item_t  item = {.value = 42};

    xylem_list_init(&list);
    xylem_list_push_back(&list, &item.node);

    xylem_list_node_t* n = xylem_list_front(&list);
    test_item_t*       recovered = xylem_list_entry(n, test_item_t, node);
    ASSERT(recovered->value == 42);
}

int main(void) {
    test_list_init();
    test_list_push_front();
    test_list_push_back();
    test_list_remove();
    test_list_entry();
    return 0;
}
