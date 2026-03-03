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

typedef int (*xylem_xrbtree_cmp_dd_fn_t)(const void* a, const void* b);
typedef int (*xylem_xrbtree_cmp_kd_fn_t)(const void* key, const void* data);

typedef struct xylem_xrbtree_s xylem_xrbtree_t;

struct xylem_xrbtree_s {
    xylem_rbtree_t             tree;
    xylem_xrbtree_cmp_dd_fn_t  cmp_dd;
    xylem_xrbtree_cmp_kd_fn_t  cmp_kd;
};

/**
 * @brief Initialize a non-intrusive red-black tree.
 *
 * @param tree    Pointer to the tree structure to initialize.
 * @param cmp_dd  Data-data comparator for insertion. Returns negative if a
 *                sorts before b, positive if after, zero if equal.
 * @param cmp_kd  Key-data comparator for lookups via xylem_xrbtree_find().
 */
extern void xylem_xrbtree_init(xylem_xrbtree_t*           tree,
                               xylem_xrbtree_cmp_dd_fn_t  cmp_dd,
                               xylem_xrbtree_cmp_kd_fn_t  cmp_kd);

/**
 * @brief Check whether the tree is empty.
 *
 * @param tree  Pointer to the tree.
 *
 * @return true if the tree contains no elements, false otherwise.
 */
extern bool xylem_xrbtree_empty(xylem_xrbtree_t* tree);

/**
 * @brief Insert an element into the tree.
 *
 * Allocates an internal node and stores the pointer to data.
 * If an equal element already exists, the insertion is rejected.
 * The caller must ensure data remains valid while in the tree.
 *
 * @param tree  Pointer to the tree.
 * @param data  Pointer to the element data to store.
 *
 * @return 0 on success, -1 on allocation failure or duplicate.
 */
extern int xylem_xrbtree_insert(xylem_xrbtree_t* tree, void* data);

/**
 * @brief Find an element by key.
 *
 * The key is compared using the same comparator provided at init.
 *
 * @param tree  Pointer to the tree.
 * @param key   Pointer to the key data to search for.
 *
 * @return Pointer to the matching element data, or NULL if not found.
 */
extern void* xylem_xrbtree_find(xylem_xrbtree_t* tree, const void* key);

/**
 * @brief Remove an element from the tree by key.
 *
 * Finds the element matching key, removes it from the tree, and frees
 * the internal node.
 *
 * @param tree  Pointer to the tree.
 * @param key   Pointer to the key data to remove.
 *
 * @return 0 on success, -1 if the key was not found.
 */
extern int xylem_xrbtree_erase(xylem_xrbtree_t* tree, const void* key);

/**
 * @brief Return a pointer to the smallest element.
 *
 * @param tree  Pointer to the tree.
 *
 * @return Pointer to the element data, or NULL if the tree is empty.
 */
extern void* xylem_xrbtree_first(xylem_xrbtree_t* tree);

/**
 * @brief Return a pointer to the largest element.
 *
 * @param tree  Pointer to the tree.
 *
 * @return Pointer to the element data, or NULL if the tree is empty.
 */
extern void* xylem_xrbtree_last(xylem_xrbtree_t* tree);

/**
 * @brief Remove and free all elements in the tree.
 *
 * @param tree  Pointer to the tree.
 */
extern void xylem_xrbtree_clear(xylem_xrbtree_t* tree);
