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

#define xylem_stack_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))

typedef struct xylem_stack_s      xylem_stack_t;
typedef struct xylem_stack_node_s xylem_stack_node_t;

struct xylem_stack_node_s {
    struct xylem_stack_node_s* next;
};

struct xylem_stack_s {
    xylem_stack_node_t* top;
    size_t              nelts;
};

/**
 * @brief Initialize a LIFO stack.
 *
 * @param stack  Pointer to the stack structure to initialize.
 */
extern void xylem_stack_init(xylem_stack_t* stack);

/**
 * @brief Check whether the stack is empty.
 *
 * @param stack  Pointer to the stack.
 *
 * @return true if the stack contains no nodes, false otherwise.
 */
extern bool xylem_stack_empty(xylem_stack_t* stack);

/**
 * @brief Return the number of nodes in the stack.
 *
 * @param stack  Pointer to the stack.
 *
 * @return Number of nodes.
 */
extern size_t xylem_stack_len(xylem_stack_t* stack);

/**
 * @brief Push a node onto the top of the stack.
 *
 * @param stack  Pointer to the stack.
 * @param node   Pointer to the intrusive node to push.
 */
extern void xylem_stack_push(xylem_stack_t* stack, xylem_stack_node_t* node);

/**
 * @brief Pop and return the top node from the stack.
 *
 * @param stack  Pointer to the stack.
 *
 * @return Pointer to the popped node, or NULL if the stack is empty.
 */
extern xylem_stack_node_t* xylem_stack_pop(xylem_stack_t* stack);

/**
 * @brief Return the top node without removing it.
 *
 * @param stack  Pointer to the stack.
 *
 * @return Pointer to the top node, or NULL if the stack is empty.
 */
extern xylem_stack_node_t* xylem_stack_peek(xylem_stack_t* stack);
