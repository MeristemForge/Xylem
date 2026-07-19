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

#include <stddef.h>

typedef struct platform_coro_s {
    void*  ptr;
    size_t size;
    void*  stack_low;
    size_t stack_size;
} platform_coro_t;

/**
 * @brief Initialize a coroutine slot memory layout.
 *
 * ptr and size define the fixed slot range [ptr, ptr + size). A NULL stack_low
 * with zero stack_size selects an external stack. Otherwise the embedded stack
 * is [stack_low, stack_low + stack_size). On Windows, the slot prefix contains
 * nonempty metadata followed by one reserved boundary page immediately before
 * the stack. The stack keeps all but its guard and top pages uncommitted.
 *
 * @param coro  Coroutine slot description.
 *
 * @return 0 on success, -1 for an invalid layout or VM operation failure.
 */
extern int platform_coro_init(const platform_coro_t* coro);

/**
 * @brief Restore a coroutine slot for reuse.
 *
 * @param coro                 Coroutine slot description.
 * @param current_stack_limit  Current native stack limit, or NULL to rebuild.
 *
 * @return 0 on success, -1 for an invalid layout or VM operation failure.
 */
extern int platform_coro_reset(
    const platform_coro_t* coro,
    void* current_stack_limit);

/**
 * @brief Return the platform initial stack-limit value used by the adapter.
 *
 * @param coro  Coroutine slot description.
 *
 * @return Platform stack-limit value, or NULL for an external stack.
 *
 * @note Unix may return stack_low even when minicoro's setter is a no-op.
 */
extern void* platform_coro_initial_stack_limit(const platform_coro_t* coro);
