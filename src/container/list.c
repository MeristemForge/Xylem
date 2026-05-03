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

#include "container/list.h"

void list_init(list_t* list) {
    list->head.prev = &list->head;
    list->head.next = &list->head;
    list->nelts = 0;
}

bool list_empty(list_t* list) {
    return list->nelts == 0;
}

size_t list_len(list_t* list) {
    return list->nelts;
}

void list_insert_head(list_t* list, list_node_t* node) {
    node->next = list->head.next;
    node->prev = &list->head;
    list->head.next->prev = node;
    list->head.next = node;
    list->nelts++;
}

void list_insert_tail(list_t* list, list_node_t* node) {
    node->prev = list->head.prev;
    node->next = &list->head;
    list->head.prev->next = node;
    list->head.prev = node;
    list->nelts++;
}

void list_remove(list_t* list, list_node_t* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = NULL;
    node->next = NULL;
    list->nelts--;
}

list_node_t* list_head(list_t* list) {
    if (list_empty(list)) {
        return NULL;
    }
    return list->head.next;
}

list_node_t* list_tail(list_t* list) {
    if (list_empty(list)) {
        return NULL;
    }
    return list->head.prev;
}

list_node_t* list_next(list_node_t* node) {
    return node->next;
}

list_node_t* list_prev(list_node_t* node) {
    return node->prev;
}

list_node_t* list_sentinel(list_t* list) {
    return &list->head;
}

void list_swap(list_t* a, list_t* b) {
    if (a == b) {
        return;
    }

    bool a_empty = (a->nelts == 0);
    bool b_empty = (b->nelts == 0);

    if (a_empty && b_empty) {
        return;
    }

    if (a_empty) {
        a->head.next = b->head.next;
        a->head.prev = b->head.prev;
        a->head.next->prev = &a->head;
        a->head.prev->next = &a->head;
        b->head.next = &b->head;
        b->head.prev = &b->head;
    } else if (b_empty) {
        b->head.next = a->head.next;
        b->head.prev = a->head.prev;
        b->head.next->prev = &b->head;
        b->head.prev->next = &b->head;
        a->head.next = &a->head;
        a->head.prev = &a->head;
    } else {
        list_node_t* tmp_next = a->head.next;
        list_node_t* tmp_prev = a->head.prev;

        a->head.next = b->head.next;
        a->head.prev = b->head.prev;
        b->head.next = tmp_next;
        b->head.prev = tmp_prev;

        a->head.next->prev = &a->head;
        a->head.prev->next = &a->head;
        b->head.next->prev = &b->head;
        b->head.prev->next = &b->head;
    }

    size_t tmp = a->nelts;
    a->nelts   = b->nelts;
    b->nelts   = tmp;
}
