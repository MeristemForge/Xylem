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
    int32_t va = *(const int32_t*)a;
    int32_t vb = *(const int32_t*)b;
    if (va < vb) {
        return -1;
    }
    if (va > vb) {
        return 1;
    }
    return 0;
}

static int _int_cmp_kd(const void* key, const void* data) {
    int32_t vk = *(const int32_t*)key;
    int32_t vd = *(const int32_t*)data;
    if (vk < vd) {
        return -1;
    }
    if (vk > vd) {
        return 1;
    }
    return 0;
}

static void test_init(void) {
    xylem_rbtree_t* tree = xylem_rbtree_create(_int_cmp_dd, _int_cmp_kd);
    ASSERT(tree != NULL);
    ASSERT(xylem_rbtree_empty(tree));
    ASSERT(xylem_rbtree_first(tree) == NULL);
    ASSERT(xylem_rbtree_last(tree) == NULL);
    xylem_rbtree_destroy(tree);
}

static void test_insert_find(void) {
    xylem_rbtree_t* tree = xylem_rbtree_create(_int_cmp_dd, _int_cmp_kd);

    int32_t vals[] = {50, 30, 70, 20, 40};
    for (int i = 0; i < 5; i++) {
        ASSERT(xylem_rbtree_insert(tree, &vals[i]) == 0);
    }

    for (int i = 0; i < 5; i++) {
        int32_t* found = (int32_t*)xylem_rbtree_find(tree, &vals[i]);
        ASSERT(found != NULL);
        ASSERT(*found == vals[i]);
    }

    int32_t missing = 99;
    ASSERT(xylem_rbtree_find(tree, &missing) == NULL);

    xylem_rbtree_destroy(tree);
}

static void test_duplicate(void) {
    xylem_rbtree_t* tree = xylem_rbtree_create(_int_cmp_dd, _int_cmp_kd);

    int32_t v = 42;
    ASSERT(xylem_rbtree_insert(tree, &v) == 0);
    ASSERT(xylem_rbtree_insert(tree, &v) == -1);

    xylem_rbtree_destroy(tree);
}

static void test_erase(void) {
    xylem_rbtree_t* tree = xylem_rbtree_create(_int_cmp_dd, _int_cmp_kd);

    int32_t vals[20];
    for (int i = 0; i < 20; i++) {
        vals[i] = i;
        xylem_rbtree_insert(tree, &vals[i]);
    }

    for (int i = 0; i < 20; i += 2) {
        ASSERT(xylem_rbtree_erase(tree, &vals[i]) == 0);
    }

    for (int i = 0; i < 20; i++) {
        int32_t* found = (int32_t*)xylem_rbtree_find(tree, &vals[i]);
        if (i % 2 == 0) {
            ASSERT(found == NULL);
        } else {
            ASSERT(found != NULL && *found == i);
        }
    }

    int32_t missing = 100;
    ASSERT(xylem_rbtree_erase(tree, &missing) == -1);

    xylem_rbtree_destroy(tree);
}

static void test_first_last(void) {
    xylem_rbtree_t* tree = xylem_rbtree_create(_int_cmp_dd, _int_cmp_kd);

    int32_t vals[] = {50, 10, 90, 30, 70};
    for (int i = 0; i < 5; i++) {
        xylem_rbtree_insert(tree, &vals[i]);
    }

    ASSERT(*(int32_t*)xylem_rbtree_first(tree) == 10);
    ASSERT(*(int32_t*)xylem_rbtree_last(tree) == 90);

    xylem_rbtree_destroy(tree);
}

static void test_clear(void) {
    xylem_rbtree_t* tree = xylem_rbtree_create(_int_cmp_dd, _int_cmp_kd);

    int32_t vals[100];
    for (int i = 0; i < 100; i++) {
        vals[i] = i;
        xylem_rbtree_insert(tree, &vals[i]);
    }

    xylem_rbtree_clear(tree);
    ASSERT(xylem_rbtree_empty(tree));
    xylem_rbtree_destroy(tree);
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
