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

static int _int_cmp_dd(const void* a, const void* b) {
    int va = *(const int*)a;
    int vb = *(const int*)b;
    if (va < vb) {
        return -1;
    }
    if (va > vb) {
        return 1;
    }
    return 0;
}

static int _int_cmp_kd(const void* key, const void* data) {
    int vk = *(const int*)key;
    int vd = *(const int*)data;
    if (vk < vd) {
        return -1;
    }
    if (vk > vd) {
        return 1;
    }
    return 0;
}

static void test_init(void) {
    xylem_xrbtree_t tree;
    xylem_xrbtree_init(&tree, _int_cmp_dd, _int_cmp_kd);
    ASSERT(xylem_xrbtree_empty(&tree));
    ASSERT(xylem_xrbtree_first(&tree) == NULL);
    ASSERT(xylem_xrbtree_last(&tree) == NULL);
}

static void test_insert_find(void) {
    xylem_xrbtree_t tree;
    xylem_xrbtree_init(&tree, _int_cmp_dd, _int_cmp_kd);

    int vals[] = {50, 30, 70, 20, 40};
    for (int i = 0; i < 5; i++) {
        ASSERT(xylem_xrbtree_insert(&tree, &vals[i]) == 0);
    }

    for (int i = 0; i < 5; i++) {
        int* found = (int*)xylem_xrbtree_find(&tree, &vals[i]);
        ASSERT(found != NULL);
        ASSERT(*found == vals[i]);
    }

    int missing = 99;
    ASSERT(xylem_xrbtree_find(&tree, &missing) == NULL);

    xylem_xrbtree_clear(&tree);
}

static void test_duplicate(void) {
    xylem_xrbtree_t tree;
    xylem_xrbtree_init(&tree, _int_cmp_dd, _int_cmp_kd);

    int v = 42;
    ASSERT(xylem_xrbtree_insert(&tree, &v) == 0);
    ASSERT(xylem_xrbtree_insert(&tree, &v) == -1);

    xylem_xrbtree_clear(&tree);
}

static void test_erase(void) {
    xylem_xrbtree_t tree;
    xylem_xrbtree_init(&tree, _int_cmp_dd, _int_cmp_kd);

    int vals[20];
    for (int i = 0; i < 20; i++) {
        vals[i] = i;
        xylem_xrbtree_insert(&tree, &vals[i]);
    }

    /* Erase even numbers */
    for (int i = 0; i < 20; i += 2) {
        ASSERT(xylem_xrbtree_erase(&tree, &vals[i]) == 0);
    }

    /* Verify only odd numbers remain */
    for (int i = 0; i < 20; i++) {
        int* found = (int*)xylem_xrbtree_find(&tree, &vals[i]);
        if (i % 2 == 0) {
            ASSERT(found == NULL);
        } else {
            ASSERT(found != NULL && *found == i);
        }
    }

    /* Erase non-existent */
    int missing = 100;
    ASSERT(xylem_xrbtree_erase(&tree, &missing) == -1);

    xylem_xrbtree_clear(&tree);
}

static void test_first_last(void) {
    xylem_xrbtree_t tree;
    xylem_xrbtree_init(&tree, _int_cmp_dd, _int_cmp_kd);

    int vals[] = {50, 10, 90, 30, 70};
    for (int i = 0; i < 5; i++) {
        xylem_xrbtree_insert(&tree, &vals[i]);
    }

    ASSERT(*(int*)xylem_xrbtree_first(&tree) == 10);
    ASSERT(*(int*)xylem_xrbtree_last(&tree) == 90);

    xylem_xrbtree_clear(&tree);
}

static void test_clear(void) {
    xylem_xrbtree_t tree;
    xylem_xrbtree_init(&tree, _int_cmp_dd, _int_cmp_kd);

    int vals[100];
    for (int i = 0; i < 100; i++) {
        vals[i] = i;
        xylem_xrbtree_insert(&tree, &vals[i]);
    }

    xylem_xrbtree_clear(&tree);
    ASSERT(xylem_xrbtree_empty(&tree));
}

int main(void) {
    test_init();
    test_insert_find();
    test_duplicate();
    test_erase();
    test_first_last();
    test_clear();
    return 0;
}
