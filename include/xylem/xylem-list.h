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

#define xylem_list_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))

typedef struct xylem_list_s      xylem_list_t;
typedef struct xylem_list_node_s xylem_list_node_t;

struct xylem_list_node_s {
    struct xylem_list_node_s* prev;
    struct xylem_list_node_s* next;
};

struct xylem_list_s {
    xylem_list_node_t head;
    size_t            nelts;
};

/**
 * @brief Initialize a doubly-linked list.
 *
 * @param list  Pointer to the list structure to initialize.
 */
extern void xylem_list_init(xylem_list_t* list);

/**
 * @brief Check whether the list is empty.
 *
 * @param list  Pointer to the list.
 *
 * @return true if the list contains no nodes, false otherwise.
 */
extern bool xylem_list_empty(xylem_list_t* list);

/**
 * @brief Return the number of nodes in the list.
 *
 * @param list  Pointer to the list.
 *
 * @return Number of nodes.
 */
extern size_t xylem_list_len(xylem_list_t* list);

/**
 * @brief Insert a node at the front of the list.
 *
 * @param list  Pointer to the list.
 * @param node  Pointer to the intrusive node to insert.
 */
extern void xylem_list_push_front(xylem_list_t* list, xylem_list_node_t* node);

/**
 * @brief Insert a node at the back of the list.
 *
 * @param list  Pointer to the list.
 * @param node  Pointer to the intrusive node to insert.
 */
extern void xylem_list_push_back(xylem_list_t* list, xylem_list_node_t* node);

/**
 * @brief Remove a specific node from the list.
 *
 * @param list  Pointer to the list.
 * @param node  Pointer to the node to remove. Must be currently in the list.
 */
extern void xylem_list_remove(xylem_list_t* list, xylem_list_node_t* node);

/**
 * @brief Return the first node in the list without removing it.
 *
 * @param list  Pointer to the list.
 *
 * @return Pointer to the first node, or NULL if the list is empty.
 */
extern xylem_list_node_t* xylem_list_front(xylem_list_t* list);

/**
 * @brief Return the last node in the list without removing it.
 *
 * @param list  Pointer to the list.
 *
 * @return Pointer to the last node, or NULL if the list is empty.
 */
extern xylem_list_node_t* xylem_list_back(xylem_list_t* list);
