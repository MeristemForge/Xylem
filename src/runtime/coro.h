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

typedef struct coro_alloc_ctx_s {
    size_t    state;
    mco_desc* desc;
    mco_desc  layout;
    void* (*alloc_cb)(size_t size, void* allocator_data);
    void (*dealloc_cb)(void* ptr, size_t size, void* allocator_data);
    void*         allocator_data;
} coro_alloc_ctx_t;

/**
 * @brief Bind a descriptor to persistent prepared-allocation state.
 *
 * @param ctx   Zero-initialized caller-owned context.
 * @param desc  Persistent descriptor whose allocator is wrapped.
 *
 * @return 0 on success, -1 for invalid arguments.
 *
 * @note ctx and desc must outlive all bound coroutines and the copool. The
 *       descriptor layout and entry function are immutable until deinit;
 *       copies may change only user_data.
 */
extern int coro_alloc_ctx_init(coro_alloc_ctx_t* ctx, mco_desc* desc);

/**
 * @brief Restore the descriptor allocator captured during initialization.
 *
 * @param ctx  Caller-owned context.
 *
 * @note Call only after all bound coroutines and the copool are destroyed.
 *       Repeated calls are permitted.
 */
extern void coro_alloc_ctx_deinit(coro_alloc_ctx_t* ctx);

/**
 * @brief Create a coroutine and prepare its platform stack state.
 *
 * A descriptor bound to a coroutine pool preserves the saved stack limit in a
 * hot slot across reuse. A cold slot has no saved limit, so the platform's
 * initial limit is installed after minicoro initialization.
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
 * @param ctx  Persistent allocation context used by every pool slot.
 *
 * @return Slot lifecycle callbacks carrying ctx as callback data.
 *
 * @note ctx must outlive the copool created with the returned callbacks.
 */
extern copool_slot_ops_t coro_get_slot_ops(coro_alloc_ctx_t* ctx);
