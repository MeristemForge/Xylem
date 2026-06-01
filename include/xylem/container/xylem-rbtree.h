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

typedef struct xylem_rbtree_s xylem_rbtree_t;

/** @brief Data-data comparator. Returns negative if a < b, zero if equal, positive if a > b. */
typedef int (*xylem_rbtree_cmp_dd_fn_t)(const void* a, const void* b);

/** @brief Key-data comparator for lookups. Returns negative if key < data, zero if match. */
typedef int (*xylem_rbtree_cmp_kd_fn_t)(const void* key, const void* data);

/**
 * @brief Create a sorted associative container with O(log n) operations.
 *
 * @param cmp_dd  Comparator for ordering data elements.
 * @param cmp_kd  Comparator for key-based lookups.
 * @return New tree, or NULL on allocation failure.
 */
extern xylem_rbtree_t* xylem_rbtree_create(xylem_rbtree_cmp_dd_fn_t cmp_dd,
                                            xylem_rbtree_cmp_kd_fn_t cmp_kd);

/**
 * @brief Destroy the tree and free all associated memory.
 *
 * @param tree  Tree to destroy.
 */
extern void xylem_rbtree_destroy(xylem_rbtree_t* tree);

/**
 * @brief Check whether the tree contains any elements.
 *
 * @param tree  Tree to check.
 * @return true if the tree is empty, false otherwise.
 */
extern bool xylem_rbtree_empty(xylem_rbtree_t* tree);

/**
 * @brief Insert a data element into the tree.
 *
 * @param tree  Tree to insert into.
 * @param data  Pointer to the data element.
 * @return 0 on success, -1 if a duplicate key already exists.
 */
extern int xylem_rbtree_insert(xylem_rbtree_t* tree, void* data);

/**
 * @brief Find an element by key.
 *
 * @param tree  Tree to search.
 * @param key   Key to look up.
 * @return Pointer to the stored data, or NULL if not found.
 */
extern void* xylem_rbtree_find(xylem_rbtree_t* tree, const void* key);

/**
 * @brief Remove an element by key.
 *
 * @param tree  Tree to remove from.
 * @param key   Key identifying the element to remove.
 * @return 0 on success, -1 if the key was not found.
 */
extern int xylem_rbtree_erase(xylem_rbtree_t* tree, const void* key);

/**
 * @brief Return the smallest element according to the comparator.
 *
 * @param tree  Tree to query.
 * @return Pointer to the data, or NULL if the tree is empty.
 */
extern void* xylem_rbtree_first(xylem_rbtree_t* tree);

/**
 * @brief Return the largest element according to the comparator.
 *
 * @param tree  Tree to query.
 * @return Pointer to the data, or NULL if the tree is empty.
 */
extern void* xylem_rbtree_last(xylem_rbtree_t* tree);

/**
 * @brief Remove all elements from the tree.
 *
 * @param tree  Tree to clear.
 */
extern void xylem_rbtree_clear(xylem_rbtree_t* tree);
