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

#include "copool.h"

#include "minicoro/minicoro.h"

/**
 * @brief Create a coroutine and prepare its platform stack state.
 *
 * @param out   Receives the coroutine pointer on success.
 * @param desc  Coroutine descriptor and allocator configuration.
 *
 * @return A minicoro result code.
 */
extern mco_result coro_create(mco_coro** out, mco_desc* desc);

/**
 * @brief Destroy a coroutine and return its slot to its allocator.
 *
 * @param co  Coroutine pointer.
 *
 * @return A minicoro result code.
 */
extern mco_result coro_destroy(mco_coro* co);

/**
 * @brief Return coroutine-pool slot lifecycle callbacks.
 *
 * @param desc  Descriptor used to describe every pool slot.
 *
 * @return Slot lifecycle callbacks carrying desc as callback data.
 *
 * @note desc must outlive the copool created with the returned callbacks.
 */
extern copool_slot_ops_t coro_get_slot_ops(const mco_desc* desc);
