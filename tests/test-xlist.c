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
    xylem_xlist_t list;
    xylem_xlist_init(&list);
    ASSERT(xylem_xlist_empty(&list));
    ASSERT(xylem_xlist_len(&list) == 0);
    ASSERT(xylem_xlist_head(&list) == NULL);
    ASSERT(xylem_xlist_tail(&list) == NULL);
}

static void test_insert_head_tail(void) {
    xylem_xlist_t list;
    xylem_xlist_init(&list);

    int a = 10, b = 20, c = 30;
    ASSERT(xylem_xlist_insert_head(&list, &a) == 0);
    ASSERT(xylem_xlist_insert_tail(&list, &b) == 0);
    ASSERT(xylem_xlist_insert_head(&list, &c) == 0);

    /* order: &c, &a, &b -> 30, 10, 20 */
    ASSERT(xylem_xlist_len(&list) == 3);
    ASSERT(*(int*)xylem_xlist_head(&list) == 30);
    ASSERT(*(int*)xylem_xlist_tail(&list) == 20);

    xylem_xlist_clear(&list);
    ASSERT(xylem_xlist_empty(&list));
}

static void test_remove(void) {
    xylem_xlist_t list;
    xylem_xlist_init(&list);

    int vals[] = {1, 2, 3, 4};
    for (int i = 0; i < 4; i++) {
        xylem_xlist_insert_tail(&list, &vals[i]);
    }

    /* remove head element */
    xylem_xlist_remove(&list, &vals[0]);
    ASSERT(*(int*)xylem_xlist_head(&list) == 2);

    /* remove tail element */
    xylem_xlist_remove(&list, &vals[3]);
    ASSERT(*(int*)xylem_xlist_tail(&list) == 3);

    /* remove middle element */
    xylem_xlist_remove(&list, &vals[1]);
    ASSERT(xylem_xlist_len(&list) == 1);
    ASSERT(*(int*)xylem_xlist_head(&list) == 3);

    /* remove non-existent element (no-op) */
    int x = 999;
    xylem_xlist_remove(&list, &x);
    ASSERT(xylem_xlist_len(&list) == 1);

    xylem_xlist_clear(&list);
}

static void test_struct_type(void) {
    typedef struct { double x; double y; } point_t;
    xylem_xlist_t list;
    xylem_xlist_init(&list);

    point_t p1 = {1.0, 2.0};
    point_t p2 = {3.0, 4.0};
    xylem_xlist_insert_tail(&list, &p1);
    xylem_xlist_insert_tail(&list, &p2);

    point_t* f = (point_t*)xylem_xlist_head(&list);
    ASSERT(f->x == 1.0 && f->y == 2.0);

    point_t* b = (point_t*)xylem_xlist_tail(&list);
    ASSERT(b->x == 3.0 && b->y == 4.0);

    xylem_xlist_clear(&list);
}

int main(void) {
    test_init();
    test_insert_head_tail();
    test_remove();
    test_struct_type();
    return 0;
}
