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

typedef struct {
    xylem_list_node_t node;
    void*             data;
} _xlist_node_t;

static inline _xlist_node_t* _xlist_alloc_node(void* data) {
    _xlist_node_t* n = malloc(sizeof(_xlist_node_t));
    if (!n) return NULL;
    n->data = data;
    return n;
}

void xylem_xlist_init(xylem_xlist_t* list) {
    xylem_list_init(&list->list);
}

bool xylem_xlist_empty(xylem_xlist_t* list) {
    return xylem_list_empty(&list->list);
}

size_t xylem_xlist_len(xylem_xlist_t* list) {
    return xylem_list_len(&list->list);
}

int xylem_xlist_insert_head(xylem_xlist_t* list, void* data) {
    _xlist_node_t* n = _xlist_alloc_node(data);
    if (!n) return -1;
    xylem_list_insert_head(&list->list, &n->node);
    return 0;
}

int xylem_xlist_insert_tail(xylem_xlist_t* list, void* data) {
    _xlist_node_t* n = _xlist_alloc_node(data);
    if (!n) return -1;
    xylem_list_insert_tail(&list->list, &n->node);
    return 0;
}

void* xylem_xlist_head(xylem_xlist_t* list) {
    xylem_list_node_t* n = xylem_list_head(&list->list);
    return n ? xylem_list_entry(n, _xlist_node_t, node)->data : NULL;
}

void* xylem_xlist_tail(xylem_xlist_t* list) {
    xylem_list_node_t* n = xylem_list_tail(&list->list);
    return n ? xylem_list_entry(n, _xlist_node_t, node)->data : NULL;
}

void xylem_xlist_remove(xylem_xlist_t* list, void* data) {
    xylem_list_node_t* sentinel = xylem_list_sentinel(&list->list);
    xylem_list_node_t* n        = xylem_list_head(&list->list);
    while (n && n != sentinel) {
        _xlist_node_t* xn = xylem_list_entry(n, _xlist_node_t, node);
        if (xn->data == data) {
            xylem_list_remove(&list->list, n);
            free(xn);
            return;
        }
        n = xylem_list_next(n);
    }
}

void xylem_xlist_clear(xylem_xlist_t* list) {
    xylem_list_node_t* n = xylem_list_head(&list->list);
    while (n) {
        _xlist_node_t* xn   = xylem_list_entry(n, _xlist_node_t, node);
        xylem_list_node_t* next = xylem_list_next(n);
        free(xn);
        n = (next == xylem_list_sentinel(&list->list)) ? NULL : next;
    }
    xylem_list_init(&list->list);
}
