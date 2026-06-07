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

#include "xylem/container/xylem-rbtree.h"
#include "container/rbtree.h"

#include <stdlib.h>

struct xylem_rbtree_s {
    rbtree_t                 tree;
    xylem_rbtree_cmp_dd_fn_t cmp_dd;
    xylem_rbtree_cmp_kd_fn_t cmp_kd;
};

typedef struct _rbtree_wrap_node_s {
    rbtree_node_t            node;
    void*                    data;
    xylem_rbtree_cmp_dd_fn_t cmp_dd;
    xylem_rbtree_cmp_kd_fn_t cmp_kd;
} _rbtree_wrap_node_t;

static int _rbtree_cmp_nn_bridge(
    const rbtree_node_t* a, const rbtree_node_t* b) {
    _rbtree_wrap_node_t* na = rbtree_entry((rbtree_node_t*)a, _rbtree_wrap_node_t, node);
    _rbtree_wrap_node_t* nb = rbtree_entry((rbtree_node_t*)b, _rbtree_wrap_node_t, node);
    return na->cmp_dd(na->data, nb->data);
}

static int _rbtree_cmp_kn_bridge(const void* key, const rbtree_node_t* n) {
    _rbtree_wrap_node_t* xn = rbtree_entry((rbtree_node_t*)n, _rbtree_wrap_node_t, node);
    return xn->cmp_kd(key, xn->data);
}

xylem_rbtree_t* xylem_rbtree_create(xylem_rbtree_cmp_dd_fn_t cmp_dd,
                                     xylem_rbtree_cmp_kd_fn_t cmp_kd) {
    xylem_rbtree_t* t = (xylem_rbtree_t*)calloc(1, sizeof(xylem_rbtree_t));
    if (!t) {
        return NULL;
    }
    rbtree_init(&t->tree, _rbtree_cmp_nn_bridge, _rbtree_cmp_kn_bridge);
    t->cmp_dd = cmp_dd;
    t->cmp_kd = cmp_kd;
    return t;
}

void xylem_rbtree_destroy(xylem_rbtree_t* tree) {
    if (!tree) {
        return;
    }
    xylem_rbtree_clear(tree);
    free(tree);
}

bool xylem_rbtree_empty(xylem_rbtree_t* tree) {
    return rbtree_empty(&tree->tree);
}

int xylem_rbtree_insert(xylem_rbtree_t* tree, void* data) {
    if (rbtree_find(&tree->tree, data)) {
        return -1;
    }
    _rbtree_wrap_node_t* n = (_rbtree_wrap_node_t*)calloc(1, sizeof(_rbtree_wrap_node_t));
    if (!n) {
        return -1;
    }
    n->data   = data;
    n->cmp_dd = tree->cmp_dd;
    n->cmp_kd = tree->cmp_kd;
    rbtree_insert(&tree->tree, &n->node);
    return 0;
}

void* xylem_rbtree_find(xylem_rbtree_t* tree, const void* key) {
    rbtree_node_t* n = rbtree_find(&tree->tree, key);
    return n ? rbtree_entry(n, _rbtree_wrap_node_t, node)->data : NULL;
}

int xylem_rbtree_erase(xylem_rbtree_t* tree, const void* key) {
    rbtree_node_t* n = rbtree_find(&tree->tree, key);
    if (!n) {
        return -1;
    }
    rbtree_remove(&tree->tree, n);
    free(rbtree_entry(n, _rbtree_wrap_node_t, node));
    return 0;
}

void* xylem_rbtree_first(xylem_rbtree_t* tree) {
    rbtree_node_t* n = rbtree_min(&tree->tree);
    return n ? rbtree_entry(n, _rbtree_wrap_node_t, node)->data : NULL;
}

void* xylem_rbtree_last(xylem_rbtree_t* tree) {
    rbtree_node_t* n = rbtree_max(&tree->tree);
    return n ? rbtree_entry(n, _rbtree_wrap_node_t, node)->data : NULL;
}

void xylem_rbtree_clear(xylem_rbtree_t* tree) {
    while (!rbtree_empty(&tree->tree)) {
        rbtree_node_t* n = rbtree_min(&tree->tree);
        rbtree_remove(&tree->tree, n);
        free(rbtree_entry(n, _rbtree_wrap_node_t, node));
    }
}
