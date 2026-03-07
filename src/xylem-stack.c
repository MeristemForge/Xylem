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

void xylem_stack_init(xylem_stack_t* stack) {
    stack->top = NULL;
    stack->nelts = 0;
}

bool xylem_stack_empty(xylem_stack_t* stack) {
    return stack->nelts == 0;
}

size_t xylem_stack_len(xylem_stack_t* stack) {
    return stack->nelts;
}

void xylem_stack_push(xylem_stack_t* stack, xylem_stack_node_t* node) {
    node->next = stack->top;
    stack->top = node;
    stack->nelts++;
}

xylem_stack_node_t* xylem_stack_pop(xylem_stack_t* stack) {
    if (stack->top == NULL) {
        return NULL;
    }
    xylem_stack_node_t* node = stack->top;
    stack->top = node->next;
    node->next = NULL;
    stack->nelts--;
    return node;
}

xylem_stack_node_t* xylem_stack_peek(xylem_stack_t* stack) {
    return stack->top;
}
