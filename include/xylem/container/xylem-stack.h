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

typedef struct xylem_stack_s xylem_stack_t;

/**
 * @brief Create a LIFO stack.
 *
 * @return New stack, or NULL on allocation failure.
 */
extern xylem_stack_t* xylem_stack_create(void);

/**
 * @brief Destroy the stack and free all associated memory.
 *
 * @param stack  Stack to destroy.
 */
extern void xylem_stack_destroy(xylem_stack_t* stack);

/**
 * @brief Check whether the stack contains any elements.
 *
 * @param stack  Stack to check.
 * @return true if the stack is empty, false otherwise.
 */
extern bool xylem_stack_empty(xylem_stack_t* stack);

/**
 * @brief Return the number of elements in the stack.
 *
 * @param stack  Stack to query.
 * @return Number of elements.
 */
extern size_t xylem_stack_len(xylem_stack_t* stack);

/**
 * @brief Push a data element onto the top of the stack.
 *
 * @param stack  Stack to push onto.
 * @param data   Pointer to the data element.
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_stack_push(xylem_stack_t* stack, void* data);

/**
 * @brief Return the top element without removing it.
 *
 * @param stack  Stack to query.
 * @return Pointer to the data, or NULL if the stack is empty.
 */
extern void* xylem_stack_peek(xylem_stack_t* stack);

/**
 * @brief Remove the top element.
 *
 * No-op if the stack is empty.
 *
 * @param stack  Stack to pop from.
 */
extern void xylem_stack_pop(xylem_stack_t* stack);

/**
 * @brief Remove all elements from the stack.
 *
 * @param stack  Stack to clear.
 */
extern void xylem_stack_clear(xylem_stack_t* stack);
