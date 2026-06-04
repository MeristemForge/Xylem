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

#include "xylem/container/xylem-list.h"
#include "container/list.h"

#include <stdlib.h>

struct xylem_list_s {
    list_t list;
};

typedef struct _list_wrap_node_s {
    list_node_t node;
    void*       data;
} _list_wrap_node_t;

static inline _list_wrap_node_t* _list_wrap_alloc(void* data) {
    _list_wrap_node_t* n = (_list_wrap_node_t*)calloc(1, sizeof(_list_wrap_node_t));
    if (!n) {
        return NULL;
    }
    n->data = data;
    return n;
}

xylem_list_t* xylem_list_create(void) {
    xylem_list_t* list = (xylem_list_t*)calloc(1, sizeof(xylem_list_t));
    if (!list) {
        return NULL;
    }
    list_init(&list->list);
    return list;
}

void xylem_list_destroy(xylem_list_t* list) {
    if (!list) {
        return;
    }
    xylem_list_clear(list);
    free(list);
}

bool xylem_list_empty(xylem_list_t* list) {
    return list_empty(&list->list);
}

size_t xylem_list_len(xylem_list_t* list) {
    return list_len(&list->list);
}

int xylem_list_insert_head(xylem_list_t* list, void* data) {
    _list_wrap_node_t* n = _list_wrap_alloc(data);
    if (!n) {
        return -1;
    }
    list_insert_head(&list->list, &n->node);
    return 0;
}

int xylem_list_insert_tail(xylem_list_t* list, void* data) {
    _list_wrap_node_t* n = _list_wrap_alloc(data);
    if (!n) {
        return -1;
    }
    list_insert_tail(&list->list, &n->node);
    return 0;
}

void* xylem_list_head(xylem_list_t* list) {
    list_node_t* n = list_head(&list->list);
    return n ? list_entry(n, _list_wrap_node_t, node)->data : NULL;
}

void* xylem_list_tail(xylem_list_t* list) {
    list_node_t* n = list_tail(&list->list);
    return n ? list_entry(n, _list_wrap_node_t, node)->data : NULL;
}

void xylem_list_remove(xylem_list_t* list, void* data) {
    list_node_t* sentinel = list_sentinel(&list->list);
    list_node_t* n        = list_head(&list->list);
    while (n && n != sentinel) {
        _list_wrap_node_t* xn = list_entry(n, _list_wrap_node_t, node);
        if (xn->data == data) {
            list_remove(&list->list, n);
            free(xn);
            return;
        }
        n = list_next(n);
    }
}

void xylem_list_clear(xylem_list_t* list) {
    list_node_t* n = list_head(&list->list);
    while (n) {
        _list_wrap_node_t* xn   = list_entry(n, _list_wrap_node_t, node);
        list_node_t*   next = list_next(n);
        free(xn);
        n = (next == list_sentinel(&list->list)) ? NULL : next;
    }
    list_init(&list->list);
}

void xylem_list_swap(xylem_list_t* a, xylem_list_t* b) {
    list_swap(&a->list, &b->list);
}
