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

typedef struct lifo_node_s {
    struct lifo_node_s* next;
} lifo_node_t;

typedef struct lifo_s {
    lifo_node_t* top;
    size_t       nelts;
} lifo_t;

#define lifo_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))

/**
 * @brief Initialize a LIFO stack.
 *
 * @param lifo  Pointer to the lifo structure to initialize.
 */
extern void lifo_init(lifo_t* lifo);

/**
 * @brief Check whether the lifo is empty.
 *
 * @param lifo  Pointer to the lifo.
 *
 * @return true if the lifo contains no nodes, false otherwise.
 */
extern bool lifo_empty(lifo_t* lifo);

/**
 * @brief Return the number of nodes in the lifo.
 *
 * @param lifo  Pointer to the lifo.
 *
 * @return Number of nodes.
 */
extern size_t lifo_len(lifo_t* lifo);

/**
 * @brief Push a node onto the top of the lifo.
 *
 * @param lifo  Pointer to the lifo.
 * @param node  Pointer to the intrusive node to push.
 */
extern void lifo_push(lifo_t* lifo, lifo_node_t* node);

/**
 * @brief Pop and return the top node from the lifo.
 *
 * @param lifo  Pointer to the lifo.
 *
 * @return Pointer to the popped node, or NULL if the lifo is empty.
 */
extern lifo_node_t* lifo_pop(lifo_t* lifo);

/**
 * @brief Return the top node without removing it.
 *
 * @param lifo  Pointer to the lifo.
 *
 * @return Pointer to the top node, or NULL if the lifo is empty.
 */
extern lifo_node_t* lifo_peek(lifo_t* lifo);
