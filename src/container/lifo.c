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

#include "container/lifo.h"

void lifo_init(lifo_t* lifo) {
    lifo->top = NULL;
    lifo->nelts = 0;
}

bool lifo_empty(lifo_t* lifo) {
    return lifo->nelts == 0;
}

size_t lifo_len(lifo_t* lifo) {
    return lifo->nelts;
}

void lifo_push(lifo_t* lifo, lifo_node_t* node) {
    node->next = lifo->top;
    lifo->top = node;
    lifo->nelts++;
}

lifo_node_t* lifo_pop(lifo_t* lifo) {
    if (lifo->top == NULL) {
        return NULL;
    }
    lifo_node_t* node = lifo->top;
    lifo->top = node->next;
    node->next = NULL;
    lifo->nelts--;
    return node;
}

lifo_node_t* lifo_peek(lifo_t* lifo) {
    return lifo->top;
}
