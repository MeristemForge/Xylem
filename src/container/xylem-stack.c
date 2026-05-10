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

#include "xylem/container/xylem-stack.h"
#include "container/lifo.h"

#include <stdlib.h>

struct xylem_stack_s {
    lifo_t lifo;
};

typedef struct {
    lifo_node_t node;
    void*       data;
} _stack_wrap_node_t;

static inline _stack_wrap_node_t* _stack_wrap_alloc(void* data) {
    _stack_wrap_node_t* n = (_stack_wrap_node_t*)calloc(1, sizeof(_stack_wrap_node_t));
    if (!n) {
        return NULL;
    }
    n->data = data;
    return n;
}

xylem_stack_t* xylem_stack_create(void) {
    xylem_stack_t* s = (xylem_stack_t*)calloc(1, sizeof(xylem_stack_t));
    if (!s) {
        return NULL;
    }
    lifo_init(&s->lifo);
    return s;
}

void xylem_stack_destroy(xylem_stack_t* stack) {
    if (!stack) {
        return;
    }
    xylem_stack_clear(stack);
    free(stack);
}

bool xylem_stack_empty(xylem_stack_t* stack) {
    return lifo_empty(&stack->lifo);
}

size_t xylem_stack_len(xylem_stack_t* stack) {
    return lifo_len(&stack->lifo);
}

int xylem_stack_push(xylem_stack_t* stack, void* data) {
    _stack_wrap_node_t* n = _stack_wrap_alloc(data);
    if (!n) {
        return -1;
    }
    lifo_push(&stack->lifo, &n->node);
    return 0;
}

void* xylem_stack_peek(xylem_stack_t* stack) {
    lifo_node_t* n = lifo_peek(&stack->lifo);
    return n ? lifo_entry(n, _stack_wrap_node_t, node)->data : NULL;
}

void xylem_stack_pop(xylem_stack_t* stack) {
    lifo_node_t* n = lifo_pop(&stack->lifo);
    if (!n) {
        return;
    }
    free(lifo_entry(n, _stack_wrap_node_t, node));
}

void xylem_stack_clear(xylem_stack_t* stack) {
    while (!lifo_empty(&stack->lifo)) {
        xylem_stack_pop(stack);
    }
}
