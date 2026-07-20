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

#include "coro.h"

#include "platform/platform-coro.h"
#include "platform/platform-vmem.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

static _Atomic size_t _coro_stack_limit_alignment;

static int _coro_stack_limit_valid(
    const mco_coro* co,
    const void*     limit) {
    uintptr_t low;
    uintptr_t high;
    uintptr_t value;
    size_t    alignment;

    if (limit == NULL) {
        return 1;
    }
    alignment = atomic_load(&_coro_stack_limit_alignment);
    if (co == NULL || co->stack_base == NULL || co->stack_size == 0 ||
        alignment == 0) {
        return 0;
    }
    low = (uintptr_t)co->stack_base;
    if (co->stack_size > (size_t)(UINTPTR_MAX - low)) {
        return 0;
    }
    high  = low + co->stack_size;
    value = (uintptr_t)limit;
    return low < value && value < high && value % alignment == 0;
}

static int _coro_slot_init_cb(void* ptr, size_t size, void* ud) {
    const mco_desc* desc = (const mco_desc*)ud;
    size_t          stack_offset;
    size_t          stack_size;

    if (ptr == NULL || size == 0 || desc == NULL || desc->func == NULL ||
        desc->coro_size == 0 || desc->stack_size == 0 ||
        desc->coro_size > size) {
        return -1;
    }

    stack_offset = mco_desc_stack_offset(desc);
    stack_size   = stack_offset != 0 ? desc->stack_size : 0;
    if (stack_offset != 0) {
        size_t alignment = platform_vmem_page_size();
        if (alignment == 0) {
            return -1;
        }
        atomic_store(&_coro_stack_limit_alignment, alignment);
    }
    return platform_coro_prepare_slot(
        ptr,
        size,
        stack_offset,
        stack_size);
}

mco_result coro_create(mco_coro** out, mco_desc* desc) {
    mco_coro*  co;
    void*      saved_stack_limit;
    size_t     stack_offset;
    mco_result result;

    if (out == NULL) {
        return MCO_INVALID_POINTER;
    }
    *out = NULL;
    if (desc == NULL || desc->alloc_cb == NULL || desc->dealloc_cb == NULL) {
        return MCO_INVALID_ARGUMENTS;
    }

    co = (mco_coro*)desc->alloc_cb(desc->coro_size, desc->allocator_data);
    if (co == NULL) {
        return MCO_OUT_OF_MEMORY;
    }
    stack_offset      = mco_desc_stack_offset(desc);
    saved_stack_limit = mco_get_stack_limit(co);
    result = mco_init(co, desc);
    if (result != MCO_SUCCESS) {
        desc->dealloc_cb(co, desc->coro_size, desc->allocator_data);
        *out = NULL;
        return result;
    }
    if (!_coro_stack_limit_valid(co, saved_stack_limit)) {
        abort();
    }
    if (saved_stack_limit == NULL && stack_offset != 0) {
        saved_stack_limit = platform_coro_initial_stack_limit(
            co->stack_base,
            co->stack_size);
        if (saved_stack_limit == NULL) {
            abort();
        }
    }
    mco_set_stack_limit(co, saved_stack_limit);
    *out = co;
    return MCO_SUCCESS;
}

mco_result coro_destroy(mco_coro* co) {
    return mco_destroy(co);
}

copool_slot_ops_t coro_get_slot_ops(const mco_desc* desc) {
    copool_slot_ops_t ops = {
        .init = _coro_slot_init_cb,
        .ud   = (void*)desc,
    };

    return ops;
}
