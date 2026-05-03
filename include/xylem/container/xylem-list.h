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

typedef struct xylem_list_s xylem_list_t;

/**
 * @brief Create a doubly-linked list.
 *
 * @return New list, or NULL on allocation failure.
 */
extern xylem_list_t* xylem_list_create(void);

/**
 * @brief Destroy the list and free all associated memory.
 *
 * @param list  List to destroy.
 */
extern void xylem_list_destroy(xylem_list_t* list);

/**
 * @brief Check whether the list contains any elements.
 *
 * @param list  List to check.
 * @return true if the list is empty, false otherwise.
 */
extern bool xylem_list_empty(xylem_list_t* list);

/**
 * @brief Return the number of elements in the list.
 *
 * @param list  List to query.
 * @return Number of elements.
 */
extern size_t xylem_list_len(xylem_list_t* list);

/**
 * @brief Insert a data element at the front of the list.
 *
 * @param list  List to insert into.
 * @param data  Pointer to the data element.
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_list_insert_head(xylem_list_t* list, void* data);

/**
 * @brief Insert a data element at the back of the list.
 *
 * @param list  List to insert into.
 * @param data  Pointer to the data element.
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_list_insert_tail(xylem_list_t* list, void* data);

/**
 * @brief Return the first element without removing it.
 *
 * @param list  List to query.
 * @return Pointer to the data, or NULL if the list is empty.
 */
extern void* xylem_list_head(xylem_list_t* list);

/**
 * @brief Return the last element without removing it.
 *
 * @param list  List to query.
 * @return Pointer to the data, or NULL if the list is empty.
 */
extern void* xylem_list_tail(xylem_list_t* list);

/**
 * @brief Remove the first occurrence of a data pointer from the list.
 *
 * No-op if the element is not found.
 *
 * @param list  List to remove from.
 * @param data  Pointer to the data element to remove.
 */
extern void xylem_list_remove(xylem_list_t* list, void* data);

/**
 * @brief Remove all elements from the list.
 *
 * @param list  List to clear.
 */
extern void xylem_list_clear(xylem_list_t* list);

/**
 * @brief Swap the contents of two lists in O(1).
 *
 * @param a  First list.
 * @param b  Second list.
 */
extern void xylem_list_swap(xylem_list_t* a, xylem_list_t* b);
