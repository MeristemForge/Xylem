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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rbtree_node_s {
    struct rbtree_node_s* parent;
    struct rbtree_node_s* right;
    struct rbtree_node_s* left;
    uint8_t               color;
} rbtree_node_t;

typedef struct rbtree_s {
    rbtree_node_t* root;
    int (*cmp_nn)(const rbtree_node_t* child, const rbtree_node_t* parent);
    int (*cmp_kn)(const void* key, const rbtree_node_t* parent);
} rbtree_t;

#define rbtree_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))

typedef int (*rbtree_cmp_nn_fn_t)(const rbtree_node_t* child, const rbtree_node_t* parent);
typedef int (*rbtree_cmp_kn_fn_t)(const void* key, const rbtree_node_t* parent);

/**
 * @brief Initialize a red-black tree.
 *
 * @param tree    Pointer to the tree structure to initialize.
 * @param cmp_nn  Node-node comparator. Returns negative if child sorts before parent.
 * @param cmp_kn  Key-node comparator for lookups via rbtree_find().
 */
extern void rbtree_init(rbtree_t* tree, rbtree_cmp_nn_fn_t cmp_nn, rbtree_cmp_kn_fn_t cmp_kn);

/**
 * @brief Insert a node into the tree.
 *
 * @param tree  Pointer to the tree.
 * @param node  Pointer to the intrusive node to insert. Recover the container
 *              with rbtree_entry().
 */
extern void rbtree_insert(rbtree_t* tree, rbtree_node_t* node);

/**
 * @brief Remove a node from the tree.
 *
 * @param tree  Pointer to the tree.
 * @param node  Pointer to the node to remove. Must be currently in the tree.
 */
extern void rbtree_remove(rbtree_t* tree, rbtree_node_t* node);

/**
 * @brief Check whether the tree is empty.
 *
 * @param tree  Pointer to the tree.
 *
 * @return true if the tree contains no nodes, false otherwise.
 */
extern bool rbtree_empty(rbtree_t* tree);

/**
 * @brief Find a node by key using the cmp_kn comparator.
 *
 * @param tree  Pointer to the tree.
 * @param key   Pointer to the key to search for.
 *
 * @return Pointer to the matching node, or NULL if not found.
 */
extern rbtree_node_t* rbtree_find(rbtree_t* tree, const void* key);

/**
 * @brief Return the in-order successor of a node.
 *
 * @param node  Pointer to the current node.
 *
 * @return Pointer to the next node, or NULL if node is the last.
 */
extern rbtree_node_t* rbtree_next(rbtree_node_t* node);

/**
 * @brief Return the in-order predecessor of a node.
 *
 * @param node  Pointer to the current node.
 *
 * @return Pointer to the previous node, or NULL if node is the first.
 */
extern rbtree_node_t* rbtree_prev(rbtree_node_t* node);

/**
 * @brief Return the smallest (leftmost) node in the tree.
 *
 * @param tree  Pointer to the tree.
 *
 * @return Pointer to the first node, or NULL if the tree is empty.
 */
extern rbtree_node_t* rbtree_min(rbtree_t* tree);

/**
 * @brief Return the largest (rightmost) node in the tree.
 *
 * @param tree  Pointer to the tree.
 *
 * @return Pointer to the last node, or NULL if the tree is empty.
 */
extern rbtree_node_t* rbtree_max(rbtree_t* tree);