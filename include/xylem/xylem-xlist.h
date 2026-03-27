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

#include "xylem/xylem-list.h"

typedef struct xylem_xlist_s xylem_xlist_t;

struct xylem_xlist_s {
    xylem_list_t list;
};

/**
 * @brief Initialize a non-intrusive doubly-linked list.
 *
 * @param list  Pointer to the list structure to initialize.
 */
extern void xylem_xlist_init(xylem_xlist_t* list);

/**
 * @brief Check whether the list is empty.
 *
 * @param list  Pointer to the list.
 *
 * @return true if the list contains no elements, false otherwise.
 */
extern bool xylem_xlist_empty(xylem_xlist_t* list);

/**
 * @brief Return the number of elements in the list.
 *
 * @param list  Pointer to the list.
 *
 * @return Number of elements.
 */
extern size_t xylem_xlist_len(xylem_xlist_t* list);

/**
 * @brief Insert an element at the head of the list.
 *
 * Allocates an internal node and stores the pointer to data.
 * The caller must ensure data remains valid while in the list.
 *
 * @param list  Pointer to the list.
 * @param data  Pointer to the element data to store.
 *
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_xlist_insert_head(xylem_xlist_t* list, void* data);

/**
 * @brief Insert an element at the tail of the list.
 *
 * Allocates an internal node and stores the pointer to data.
 * The caller must ensure data remains valid while in the list.
 *
 * @param list  Pointer to the list.
 * @param data  Pointer to the element data to store.
 *
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_xlist_insert_tail(xylem_xlist_t* list, void* data);

/**
 * @brief Return a pointer to the head element without removing it.
 *
 * @param list  Pointer to the list.
 *
 * @return Pointer to the element data, or NULL if the list is empty.
 */
extern void* xylem_xlist_head(xylem_xlist_t* list);

/**
 * @brief Return a pointer to the tail element without removing it.
 *
 * @param list  Pointer to the list.
 *
 * @return Pointer to the element data, or NULL if the list is empty.
 */
extern void* xylem_xlist_tail(xylem_xlist_t* list);

/**
 * @brief Remove an element from the list by its data pointer.
 *
 * Traverses the list to find the node storing the given data pointer,
 * removes it, and frees the internal node. No-op if data is not found.
 *
 * @param list  Pointer to the list.
 * @param data  Pointer to the element data to remove.
 */
extern void xylem_xlist_remove(xylem_xlist_t* list, void* data);

/**
 * @brief Remove and free all elements in the list.
 *
 * @param list  Pointer to the list.
 */
extern void xylem_xlist_clear(xylem_xlist_t* list);
