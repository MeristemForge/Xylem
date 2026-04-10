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

typedef struct test_entry_s {
    int32_t              key;
    xylem_rbtree_node_t  node;
} test_entry_t;

static int _cmp_nn(const xylem_rbtree_node_t* a, const xylem_rbtree_node_t* b) {
    const test_entry_t* ea = xylem_rbtree_entry(a, test_entry_t, node);
    const test_entry_t* eb = xylem_rbtree_entry(b, test_entry_t, node);
    if (ea->key < eb->key) {
        return -1;
    }
    if (ea->key > eb->key) {
        return 1;
    }
    return 0;
}

static int _cmp_kn(const void* key, const xylem_rbtree_node_t* n) {
    int                 k = *(const int*)key;
    const test_entry_t* e = xylem_rbtree_entry(n, test_entry_t, node);
    if (k < e->key) {
        return -1;
    }
    if (k > e->key) {
        return 1;
    }
    return 0;
}

/* Validate red-black tree properties recursively.
 * Returns black-height on success, -1 on violation. */
static int _validate_rbtree(xylem_rbtree_node_t* node, xylem_rbtree_node_t* parent) {
    if (!node) {
        return 1;
    }

    /* Parent pointer check */
    if (node->parent != parent) {
        return -1;
    }

    /* Red node must not have red children */
    if (node->color == 0) { /* RED */
        if ((node->left && node->left->color == 0) ||
            (node->right && node->right->color == 0))
            return -1;
    }

    int lh = _validate_rbtree(node->left, node);
    int rh = _validate_rbtree(node->right, node);

    if (lh == -1 || rh == -1) {
        return -1;
    }
    /* Black-height must be equal on both sides */
    if (lh != rh) {
        return -1;
    }

    return lh + (node->color == 1 ? 1 : 0); /* BLACK = 1 */
}

static bool _validate_tree(xylem_rbtree_t* tree) {
    if (!tree->root) {
        return true;
    }
    /* Root must be black */
    if (tree->root->color != 1) {
        return false;
    }
    return _validate_rbtree(tree->root, NULL) != -1;
}

/* Test init: empty tree with NULL root. */
static void test_init(void) {
    xylem_rbtree_t tree;
    xylem_rbtree_init(&tree, _cmp_nn, _cmp_kn);
    ASSERT(xylem_rbtree_empty(&tree));
    ASSERT(xylem_rbtree_first(&tree) == NULL);
    ASSERT(xylem_rbtree_last(&tree) == NULL);
}

/* Test single insert and find. */
static void test_insert_single(void) {
    xylem_rbtree_t tree;
    test_entry_t   e = {.key = 42};

    xylem_rbtree_init(&tree, _cmp_nn, _cmp_kn);
    xylem_rbtree_insert(&tree, &e.node);

    ASSERT(!xylem_rbtree_empty(&tree));
    ASSERT(xylem_rbtree_first(&tree) == &e.node);
    ASSERT(xylem_rbtree_last(&tree) == &e.node);

    int k = 42;
    ASSERT(xylem_rbtree_find(&tree, &k) == &e.node);

    k = 99;
    ASSERT(xylem_rbtree_find(&tree, &k) == NULL);

    ASSERT(_validate_tree(&tree));
}

/* Test in-order traversal with next/prev. */
static void test_ordered_traversal(void) {
    xylem_rbtree_t tree;
    test_entry_t   entries[7];
    int            keys[] = {50, 30, 70, 20, 40, 60, 80};

    xylem_rbtree_init(&tree, _cmp_nn, _cmp_kn);
    for (int i = 0; i < 7; i++) {
        entries[i].key = keys[i];
        xylem_rbtree_insert(&tree, &entries[i].node);
    }

    /* Forward: 20, 30, 40, 50, 60, 70, 80 */
    int expected[] = {20, 30, 40, 50, 60, 70, 80};
    xylem_rbtree_node_t* n = xylem_rbtree_first(&tree);
    for (int i = 0; i < 7; i++) {
        ASSERT(n != NULL);
        test_entry_t* e = xylem_rbtree_entry(n, test_entry_t, node);
        ASSERT(e->key == expected[i]);
        n = xylem_rbtree_next(n);
    }
    ASSERT(n == NULL);

    /* Backward: 80, 70, 60, 50, 40, 30, 20 */
    n = xylem_rbtree_last(&tree);
    for (int i = 6; i >= 0; i--) {
        ASSERT(n != NULL);
        test_entry_t* e = xylem_rbtree_entry(n, test_entry_t, node);
        ASSERT(e->key == expected[i]);
        n = xylem_rbtree_prev(n);
    }
    ASSERT(n == NULL);

    ASSERT(_validate_tree(&tree));
}

/* Test duplicate key insertion is rejected. */
static void test_duplicate_insert(void) {
    xylem_rbtree_t tree;
    test_entry_t   a = {.key = 10};
    test_entry_t   b = {.key = 10};

    xylem_rbtree_init(&tree, _cmp_nn, _cmp_kn);
    xylem_rbtree_insert(&tree, &a.node);
    xylem_rbtree_insert(&tree, &b.node);

    /* Only the first should be in the tree */
    int k = 10;
    ASSERT(xylem_rbtree_find(&tree, &k) == &a.node);
    ASSERT(_validate_tree(&tree));
}

/* Test erase leaf, internal, and root nodes. */
static void test_erase(void) {
    xylem_rbtree_t tree;
    test_entry_t   entries[5];
    int            keys[] = {10, 20, 30, 40, 50};

    xylem_rbtree_init(&tree, _cmp_nn, _cmp_kn);
    for (int i = 0; i < 5; i++) {
        entries[i].key = keys[i];
        xylem_rbtree_insert(&tree, &entries[i].node);
    }
    ASSERT(_validate_tree(&tree));

    /* Erase a leaf-ish node */
    xylem_rbtree_erase(&tree, &entries[4].node); /* 50 */
    int k = 50;
    ASSERT(xylem_rbtree_find(&tree, &k) == NULL);
    ASSERT(_validate_tree(&tree));

    /* Erase an internal node */
    xylem_rbtree_erase(&tree, &entries[1].node); /* 20 */
    k = 20;
    ASSERT(xylem_rbtree_find(&tree, &k) == NULL);
    ASSERT(_validate_tree(&tree));

    /* Erase root */
    xylem_rbtree_node_t* root = tree.root;
    xylem_rbtree_erase(&tree, root);
    ASSERT(_validate_tree(&tree));

    /* Erase remaining */
    while (!xylem_rbtree_empty(&tree)) {
        xylem_rbtree_erase(&tree, xylem_rbtree_first(&tree));
        ASSERT(_validate_tree(&tree));
    }
    ASSERT(xylem_rbtree_empty(&tree));
}

/* Stress test: insert 0..N-1 in ascending order, validate, erase all. */
static void test_ascending_insert(void) {
    enum { N = 100 };
    xylem_rbtree_t tree;
    test_entry_t   entries[N];

    xylem_rbtree_init(&tree, _cmp_nn, _cmp_kn);
    for (int i = 0; i < N; i++) {
        entries[i].key = i;
        xylem_rbtree_insert(&tree, &entries[i].node);
    }
    ASSERT(_validate_tree(&tree));

    /* first/last */
    test_entry_t* first = xylem_rbtree_entry(xylem_rbtree_first(&tree), test_entry_t, node);
    test_entry_t* last  = xylem_rbtree_entry(xylem_rbtree_last(&tree), test_entry_t, node);
    ASSERT(first->key == 0);
    ASSERT(last->key == N - 1);

    /* Find every key */
    for (int i = 0; i < N; i++) {
        ASSERT(xylem_rbtree_find(&tree, &i) == &entries[i].node);
    }

    /* Erase all from first */
    for (int i = 0; i < N; i++) {
        xylem_rbtree_erase(&tree, &entries[i].node);
    }
    ASSERT(xylem_rbtree_empty(&tree));
}

/* Stress test: insert N..1 in descending order. */
static void test_descending_insert(void) {
    enum { N = 100 };
    xylem_rbtree_t tree;
    test_entry_t   entries[N];

    xylem_rbtree_init(&tree, _cmp_nn, _cmp_kn);
    for (int i = 0; i < N; i++) {
        entries[i].key = N - 1 - i;
        xylem_rbtree_insert(&tree, &entries[i].node);
    }
    ASSERT(_validate_tree(&tree));

    /* In-order traversal should yield 0..N-1 */
    xylem_rbtree_node_t* n = xylem_rbtree_first(&tree);
    for (int i = 0; i < N; i++) {
        ASSERT(n != NULL);
        test_entry_t* e = xylem_rbtree_entry(n, test_entry_t, node);
        ASSERT(e->key == i);
        n = xylem_rbtree_next(n);
    }
    ASSERT(n == NULL);
}

/* Test erase every other node, then remaining. */
static void test_erase_alternating(void) {
    enum { N = 50 };
    xylem_rbtree_t tree;
    test_entry_t   entries[N];

    xylem_rbtree_init(&tree, _cmp_nn, _cmp_kn);
    for (int i = 0; i < N; i++) {
        entries[i].key = i;
        xylem_rbtree_insert(&tree, &entries[i].node);
    }

    /* Erase even keys */
    for (int i = 0; i < N; i += 2) {
        xylem_rbtree_erase(&tree, &entries[i].node);
        ASSERT(_validate_tree(&tree));
    }

    /* Only odd keys remain */
    for (int i = 0; i < N; i++) {
        int found = (xylem_rbtree_find(&tree, &i) != NULL);
        ASSERT(found == (i % 2 != 0));
    }

    /* Erase remaining */
    while (!xylem_rbtree_empty(&tree)) {
        xylem_rbtree_erase(&tree, xylem_rbtree_first(&tree));
        ASSERT(_validate_tree(&tree));
    }
}

int main(void) {
    test_init();
    test_insert_single();
    test_ordered_traversal();
    test_duplicate_insert();
    test_erase();
    test_ascending_insert();
    test_descending_insert();
    test_erase_alternating();
    return 0;
}
