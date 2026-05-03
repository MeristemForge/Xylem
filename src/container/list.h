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

typedef struct list_node_s {
    struct list_node_s* prev;
    struct list_node_s* next;
} list_node_t;

typedef struct list_s {
    list_node_t head;
    size_t      nelts;
} list_t;

#define list_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))

/**
 * @brief Initialize a doubly-linked list.
 *
 * @param list  Pointer to the list structure to initialize.
 */
extern void list_init(list_t* list);

/**
 * @brief Check whether the list is empty.
 *
 * @param list  Pointer to the list.
 *
 * @return true if the list contains no nodes, false otherwise.
 */
extern bool list_empty(list_t* list);

/**
 * @brief Return the number of nodes in the list.
 *
 * @param list  Pointer to the list.
 *
 * @return Number of nodes.
 */
extern size_t list_len(list_t* list);

/**
 * @brief Insert a node at the head of the list.
 *
 * @param list  Pointer to the list.
 * @param node  Pointer to the intrusive node to insert.
 */
extern void list_insert_head(list_t* list, list_node_t* node);

/**
 * @brief Insert a node at the tail of the list.
 *
 * @param list  Pointer to the list.
 * @param node  Pointer to the intrusive node to insert.
 */
extern void list_insert_tail(list_t* list, list_node_t* node);

/**
 * @brief Remove a specific node from the list.
 *
 * @param list  Pointer to the list.
 * @param node  Pointer to the node to remove. Must be currently in the list.
 */
extern void list_remove(list_t* list, list_node_t* node);

/**
 * @brief Return the first node in the list without removing it.
 *
 * @param list  Pointer to the list.
 *
 * @return Pointer to the first node, or NULL if the list is empty.
 */
extern list_node_t* list_head(list_t* list);

/**
 * @brief Return the last node in the list without removing it.
 *
 * @param list  Pointer to the list.
 *
 * @return Pointer to the last node, or NULL if the list is empty.
 */
extern list_node_t* list_tail(list_t* list);

/**
 * @brief Return the in-order successor of a node.
 *
 * Returns the sentinel (head) when the end of the list is reached.
 * Use list_sentinel() to detect the end.
 *
 * @param node  Pointer to the current node.
 *
 * @return Pointer to the next node.
 */
extern list_node_t* list_next(list_node_t* node);

/**
 * @brief Return the in-order predecessor of a node.
 *
 * Returns the sentinel (head) when the beginning of the list is reached.
 * Use list_sentinel() to detect the end.
 *
 * @param node  Pointer to the current node.
 *
 * @return Pointer to the previous node.
 */
extern list_node_t* list_prev(list_node_t* node);

/**
 * @brief Return the sentinel node of the list.
 *
 * The sentinel is the internal head node used as a boundary marker.
 * Comparing a traversal pointer against the sentinel detects the
 * end of iteration.
 *
 * @param list  Pointer to the list.
 *
 * @return Pointer to the sentinel node.
 */
extern list_node_t* list_sentinel(list_t* list);

/**
 * @brief Swap the contents of two lists.
 *
 * After the call, list a holds the elements previously in b and vice versa.
 * Useful for atomically draining a list: swap with an empty list, then
 * process the non-empty one.
 *
 * @param a  Pointer to the first list.
 * @param b  Pointer to the second list.
 */
extern void list_swap(list_t* a, list_t* b);

