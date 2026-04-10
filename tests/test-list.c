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
    int32_t            value;
    xylem_list_node_t  node;
} test_item_t;

static void test_list_init(void) {
    xylem_list_t list;
    xylem_list_init(&list);
    ASSERT(xylem_list_empty(&list) == true);
    ASSERT(xylem_list_len(&list) == 0);
    ASSERT(xylem_list_head(&list) == NULL);
    ASSERT(xylem_list_tail(&list) == NULL);
}

static void test_list_insert_head(void) {
    xylem_list_t list;
    test_item_t  a = {.value = 1};
    test_item_t  b = {.value = 2};

    xylem_list_init(&list);
    xylem_list_insert_head(&list, &a.node);
    ASSERT(xylem_list_len(&list) == 1);
    ASSERT(xylem_list_head(&list) == &a.node);
    ASSERT(xylem_list_tail(&list) == &a.node);

    xylem_list_insert_head(&list, &b.node);
    ASSERT(xylem_list_len(&list) == 2);
    ASSERT(xylem_list_head(&list) == &b.node);
    ASSERT(xylem_list_tail(&list) == &a.node);
}

static void test_list_insert_tail(void) {
    xylem_list_t list;
    test_item_t  a = {.value = 1};
    test_item_t  b = {.value = 2};

    xylem_list_init(&list);
    xylem_list_insert_tail(&list, &a.node);
    ASSERT(xylem_list_len(&list) == 1);

    xylem_list_insert_tail(&list, &b.node);
    ASSERT(xylem_list_len(&list) == 2);
    ASSERT(xylem_list_head(&list) == &a.node);
    ASSERT(xylem_list_tail(&list) == &b.node);
}

static void test_list_remove(void) {
    xylem_list_t list;
    test_item_t  a = {.value = 1};
    test_item_t  b = {.value = 2};
    test_item_t  c = {.value = 3};

    xylem_list_init(&list);
    xylem_list_insert_tail(&list, &a.node);
    xylem_list_insert_tail(&list, &b.node);
    xylem_list_insert_tail(&list, &c.node);

    /* Remove middle node */
    xylem_list_remove(&list, &b.node);
    ASSERT(xylem_list_len(&list) == 2);
    ASSERT(xylem_list_head(&list) == &a.node);
    ASSERT(xylem_list_tail(&list) == &c.node);

    /* Remove front node */
    xylem_list_remove(&list, &a.node);
    ASSERT(xylem_list_len(&list) == 1);
    ASSERT(xylem_list_head(&list) == &c.node);

    /* Remove last node */
    xylem_list_remove(&list, &c.node);
    ASSERT(xylem_list_empty(&list) == true);
}

static void test_list_entry(void) {
    xylem_list_t list;
    test_item_t  item = {.value = 42};

    xylem_list_init(&list);
    xylem_list_insert_tail(&list, &item.node);

    xylem_list_node_t* n = xylem_list_head(&list);
    test_item_t*       recovered = xylem_list_entry(n, test_item_t, node);
    ASSERT(recovered->value == 42);
}

static void test_list_next_prev_sentinel(void) {
    xylem_list_t list;
    test_item_t  a = {.value = 1};
    test_item_t  b = {.value = 2};
    test_item_t  c = {.value = 3};

    xylem_list_init(&list);
    xylem_list_insert_tail(&list, &a.node);
    xylem_list_insert_tail(&list, &b.node);
    xylem_list_insert_tail(&list, &c.node);

    /* Forward traversal */
    xylem_list_node_t* n = xylem_list_head(&list);
    ASSERT(n == &a.node);
    n = xylem_list_next(n);
    ASSERT(n == &b.node);
    n = xylem_list_next(n);
    ASSERT(n == &c.node);
    n = xylem_list_next(n);
    ASSERT(n == xylem_list_sentinel(&list));

    /* Backward traversal */
    n = xylem_list_tail(&list);
    ASSERT(n == &c.node);
    n = xylem_list_prev(n);
    ASSERT(n == &b.node);
    n = xylem_list_prev(n);
    ASSERT(n == &a.node);
    n = xylem_list_prev(n);
    ASSERT(n == xylem_list_sentinel(&list));
}

static void test_list_swap(void) {
    xylem_list_t l1, l2;
    test_item_t  a = {.value = 1};
    test_item_t  b = {.value = 2};
    test_item_t  c = {.value = 3};

    xylem_list_init(&l1);
    xylem_list_init(&l2);

    /* Swap two empty lists */
    xylem_list_swap(&l1, &l2);
    ASSERT(xylem_list_empty(&l1));
    ASSERT(xylem_list_empty(&l2));

    /* Swap non-empty with empty */
    xylem_list_insert_tail(&l1, &a.node);
    xylem_list_insert_tail(&l1, &b.node);
    xylem_list_swap(&l1, &l2);
    ASSERT(xylem_list_empty(&l1));
    ASSERT(xylem_list_len(&l2) == 2);
    ASSERT(xylem_list_head(&l2) == &a.node);
    ASSERT(xylem_list_tail(&l2) == &b.node);

    /* Swap non-empty with non-empty */
    xylem_list_insert_tail(&l1, &c.node);
    xylem_list_swap(&l1, &l2);
    ASSERT(xylem_list_len(&l1) == 2);
    ASSERT(xylem_list_len(&l2) == 1);
    ASSERT(xylem_list_head(&l1) == &a.node);
    ASSERT(xylem_list_tail(&l1) == &b.node);
    ASSERT(xylem_list_head(&l2) == &c.node);
}

int main(void) {
    test_list_init();
    test_list_insert_head();
    test_list_insert_tail();
    test_list_remove();
    test_list_entry();
    test_list_next_prev_sentinel();
    test_list_swap();
    return 0;
}
