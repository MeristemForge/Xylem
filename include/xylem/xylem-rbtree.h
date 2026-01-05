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

_Pragma("once")

#include "xylem.h"

#define xylem_rbtree_entry(x, t, m) ((t*)((char*)(x)-offsetof(t, m)))

typedef struct xylem_rbtree_node_s xylem_rbtree_node_t;
typedef struct xylem_rbtree_s      xylem_rbtree_t;

typedef int (*xylem_rbtree_cmp_nn_fn_t)(const xylem_rbtree_node_t* child, const xylem_rbtree_node_t* parent);
typedef int (*xylem_rbtree_cmp_kn_fn_t)(const void* key, const xylem_rbtree_node_t* parent);

struct xylem_rbtree_node_s {
    struct xylem_rbtree_node_s* parent;
    struct xylem_rbtree_node_s* right;
    struct xylem_rbtree_node_s* left;
    char                        color;
};

struct xylem_rbtree_s {
    xylem_rbtree_node_t* root;
    xylem_rbtree_cmp_nn_fn_t cmp_nn;
    xylem_rbtree_cmp_kn_fn_t cmp_kn;
};

extern void xylem_rbtree_init(xylem_rbtree_t* tree, xylem_rbtree_cmp_nn_fn_t cmp_nn, xylem_rbtree_cmp_kn_fn_t cmp_kn);
extern void xylem_rbtree_insert(xylem_rbtree_t* tree, xylem_rbtree_node_t* node);
extern void xylem_rbtree_erase(xylem_rbtree_t* tree, xylem_rbtree_node_t* node);
extern bool xylem_rbtree_empty(xylem_rbtree_t* tree);
extern xylem_rbtree_node_t* xylem_rbtree_find(xylem_rbtree_t* tree, const void* key);
extern xylem_rbtree_node_t* xylem_rbtree_next(xylem_rbtree_node_t* node);
extern xylem_rbtree_node_t* xylem_rbtree_prev(xylem_rbtree_node_t* node);
extern xylem_rbtree_node_t* xylem_rbtree_first(xylem_rbtree_t* tree);
extern xylem_rbtree_node_t* xylem_rbtree_last(xylem_rbtree_t* tree);