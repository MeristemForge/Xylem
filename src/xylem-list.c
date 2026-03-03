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

void xylem_list_init(xylem_list_t* list) {
    list->head.prev = &list->head;
    list->head.next = &list->head;
    list->nelts = 0;
}

bool xylem_list_empty(xylem_list_t* list) {
    return list->nelts == 0;
}

size_t xylem_list_len(xylem_list_t* list) {
    return list->nelts;
}

void xylem_list_push_front(xylem_list_t* list, xylem_list_node_t* node) {
    node->next = list->head.next;
    node->prev = &list->head;
    list->head.next->prev = node;
    list->head.next = node;
    list->nelts++;
}

void xylem_list_push_back(xylem_list_t* list, xylem_list_node_t* node) {
    node->prev = list->head.prev;
    node->next = &list->head;
    list->head.prev->next = node;
    list->head.prev = node;
    list->nelts++;
}

void xylem_list_remove(xylem_list_t* list, xylem_list_node_t* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = NULL;
    node->next = NULL;
    list->nelts--;
}

xylem_list_node_t* xylem_list_front(xylem_list_t* list) {
    if (xylem_list_empty(list)) return NULL;
    return list->head.next;
}

xylem_list_node_t* xylem_list_back(xylem_list_t* list) {
    if (xylem_list_empty(list)) return NULL;
    return list->head.prev;
}

void xylem_list_swap(xylem_list_t* a, xylem_list_t* b) {
    xylem_list_t tmp;

    /* Save a's state. */
    tmp.head  = a->head;
    tmp.nelts = a->nelts;

    /* Move b into a. */
    if (b->nelts > 0) {
        a->head        = b->head;
        a->head.next->prev = &a->head;
        a->head.prev->next = &a->head;
    } else {
        a->head.prev = &a->head;
        a->head.next = &a->head;
    }
    a->nelts = b->nelts;

    /* Move saved a into b. */
    if (tmp.nelts > 0) {
        b->head        = tmp.head;
        b->head.next->prev = &b->head;
        b->head.prev->next = &b->head;
    } else {
        b->head.prev = &b->head;
        b->head.next = &b->head;
    }
    b->nelts = tmp.nelts;
}
