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

typedef struct {
    xylem_rbtree_node_t        node;
    void*                      data;
    xylem_xrbtree_cmp_dd_fn_t  cmp_dd;
    xylem_xrbtree_cmp_kd_fn_t  cmp_kd;
} _xrbtree_node_t;

static int _xrbtree_cmp_nn_bridge(
    const xylem_rbtree_node_t* a, const xylem_rbtree_node_t* b) {
    _xrbtree_node_t* na = xylem_rbtree_entry((xylem_rbtree_node_t*)a, _xrbtree_node_t, node);
    _xrbtree_node_t* nb = xylem_rbtree_entry((xylem_rbtree_node_t*)b, _xrbtree_node_t, node);
    return na->cmp_dd(na->data, nb->data);
}

static int
_xrbtree_cmp_kn_bridge(const void* key, const xylem_rbtree_node_t* n) {
    _xrbtree_node_t* xn = xylem_rbtree_entry((xylem_rbtree_node_t*)n, _xrbtree_node_t, node);
    return xn->cmp_kd(key, xn->data);
}

void xylem_xrbtree_init(xylem_xrbtree_t*           tree,
                        xylem_xrbtree_cmp_dd_fn_t  cmp_dd,
                        xylem_xrbtree_cmp_kd_fn_t  cmp_kd) {
    xylem_rbtree_init(&tree->tree, _xrbtree_cmp_nn_bridge, _xrbtree_cmp_kn_bridge);
    tree->cmp_dd = cmp_dd;
    tree->cmp_kd = cmp_kd;
}

bool xylem_xrbtree_empty(xylem_xrbtree_t* tree) {
    return xylem_rbtree_empty(&tree->tree);
}

int xylem_xrbtree_insert(xylem_xrbtree_t* tree, void* data) {
    if (xylem_rbtree_find(&tree->tree, data)) {
        return -1;
    }
    _xrbtree_node_t* n = malloc(sizeof(_xrbtree_node_t));
    if (!n) return -1;
    n->data   = data;
    n->cmp_dd = tree->cmp_dd;
    n->cmp_kd = tree->cmp_kd;
    xylem_rbtree_insert(&tree->tree, &n->node);
    return 0;
}

void* xylem_xrbtree_find(xylem_xrbtree_t* tree, const void* key) {
    xylem_rbtree_node_t* n = xylem_rbtree_find(&tree->tree, key);
    return n ? xylem_rbtree_entry(n, _xrbtree_node_t, node)->data : NULL;
}

int xylem_xrbtree_erase(xylem_xrbtree_t* tree, const void* key) {
    xylem_rbtree_node_t* n = xylem_rbtree_find(&tree->tree, key);
    if (!n) return -1;
    xylem_rbtree_erase(&tree->tree, n);
    free(xylem_rbtree_entry(n, _xrbtree_node_t, node));
    return 0;
}

void* xylem_xrbtree_first(xylem_xrbtree_t* tree) {
    xylem_rbtree_node_t* n = xylem_rbtree_first(&tree->tree);
    return n ? xylem_rbtree_entry(n, _xrbtree_node_t, node)->data : NULL;
}

void* xylem_xrbtree_last(xylem_xrbtree_t* tree) {
    xylem_rbtree_node_t* n = xylem_rbtree_last(&tree->tree);
    return n ? xylem_rbtree_entry(n, _xrbtree_node_t, node)->data : NULL;
}

void xylem_xrbtree_clear(xylem_xrbtree_t* tree) {
    while (!xylem_rbtree_empty(&tree->tree)) {
        xylem_rbtree_node_t* n = xylem_rbtree_first(&tree->tree);
        xylem_rbtree_erase(&tree->tree, n);
        free(xylem_rbtree_entry(n, _xrbtree_node_t, node));
    }
}
