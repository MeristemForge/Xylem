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

#include "xylem/xylem-xstack.h"

#include <stdlib.h>

typedef struct {
    xylem_stack_node_t node;
    void*              data;
} _xstack_node_t;

static inline _xstack_node_t* _xstack_alloc_node(void* data) {
    _xstack_node_t* n = malloc(sizeof(_xstack_node_t));
    if (!n) {
        return NULL;
    }
    n->data = data;
    return n;
}

void xylem_xstack_init(xylem_xstack_t* stack) {
    xylem_stack_init(&stack->stack);
}

bool xylem_xstack_empty(xylem_xstack_t* stack) {
    return xylem_stack_empty(&stack->stack);
}

size_t xylem_xstack_len(xylem_xstack_t* stack) {
    return xylem_stack_len(&stack->stack);
}

int xylem_xstack_push(xylem_xstack_t* stack, void* data) {
    _xstack_node_t* n = _xstack_alloc_node(data);
    if (!n) {
        return -1;
    }
    xylem_stack_push(&stack->stack, &n->node);
    return 0;
}

void* xylem_xstack_peek(xylem_xstack_t* stack) {
    xylem_stack_node_t* n = xylem_stack_peek(&stack->stack);
    return n ? xylem_stack_entry(n, _xstack_node_t, node)->data : NULL;
}

void xylem_xstack_pop(xylem_xstack_t* stack) {
    xylem_stack_node_t* n = xylem_stack_pop(&stack->stack);
    if (!n) {
        return;
    }
    free(xylem_stack_entry(n, _xstack_node_t, node));
}

void xylem_xstack_clear(xylem_xstack_t* stack) {
    while (!xylem_stack_empty(&stack->stack)) {
        xylem_xstack_pop(stack);
    }
}
