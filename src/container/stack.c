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

#include "container/stack.h"

void stack_init(stack_t* stack) {
    stack->top = NULL;
    stack->nelts = 0;
}

bool stack_empty(stack_t* stack) {
    return stack->nelts == 0;
}

size_t stack_len(stack_t* stack) {
    return stack->nelts;
}

void stack_push(stack_t* stack, stack_node_t* node) {
    node->next = stack->top;
    stack->top = node;
    stack->nelts++;
}

stack_node_t* stack_pop(stack_t* stack) {
    if (stack->top == NULL) {
        return NULL;
    }
    stack_node_t* node = stack->top;
    stack->top = node->next;
    node->next = NULL;
    stack->nelts--;
    return node;
}

stack_node_t* stack_peek(stack_t* stack) {
    return stack->top;
}
