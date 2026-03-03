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

#include "xylem.h"

typedef struct xylem_xstack_s xylem_xstack_t;

struct xylem_xstack_s {
    xylem_stack_t stack;
};

/**
 * @brief Initialize a non-intrusive LIFO stack.
 *
 * @param stack  Pointer to the stack structure to initialize.
 */
extern void xylem_xstack_init(xylem_xstack_t* stack);

/**
 * @brief Check whether the stack is empty.
 *
 * @param stack  Pointer to the stack.
 *
 * @return true if the stack contains no elements, false otherwise.
 */
extern bool xylem_xstack_empty(xylem_xstack_t* stack);

/**
 * @brief Return the number of elements in the stack.
 *
 * @param stack  Pointer to the stack.
 *
 * @return Number of elements.
 */
extern size_t xylem_xstack_len(xylem_xstack_t* stack);

/**
 * @brief Push an element onto the top of the stack.
 *
 * Allocates an internal node and stores the pointer to data.
 * The caller must ensure data remains valid while in the stack.
 *
 * @param stack  Pointer to the stack.
 * @param data   Pointer to the element data to store.
 *
 * @return 0 on success, -1 on allocation failure.
 */
extern int xylem_xstack_push(xylem_xstack_t* stack, void* data);

/**
 * @brief Return a pointer to the top element without removing it.
 *
 * @param stack  Pointer to the stack.
 *
 * @return Pointer to the element data, or NULL if the stack is empty.
 */
extern void* xylem_xstack_peek(xylem_xstack_t* stack);

/**
 * @brief Remove and free the top element.
 *
 * No-op if the stack is empty.
 *
 * @param stack  Pointer to the stack.
 */
extern void xylem_xstack_pop(xylem_xstack_t* stack);

/**
 * @brief Remove and free all elements in the stack.
 *
 * @param stack  Pointer to the stack.
 */
extern void xylem_xstack_clear(xylem_xstack_t* stack);
