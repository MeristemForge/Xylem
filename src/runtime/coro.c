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

static int _coro_platform(
    void*            ptr,
    size_t           size,
    const mco_desc*  desc,
    platform_coro_t* platform) {
    uintptr_t base;
    size_t    offset;

    if (ptr == NULL || size == 0 || desc == NULL || platform == NULL ||
        desc->func == NULL || desc->coro_size == 0 || desc->stack_size == 0 ||
        desc->coro_size > size) {
        return -1;
    }

    base = (uintptr_t)ptr;
    if (size > (size_t)(UINTPTR_MAX - base)) {
        return -1;
    }

    platform->ptr        = ptr;
    platform->size       = size;
    platform->stack_low  = NULL;
    platform->stack_size = 0;

    offset = mco_desc_stack_offset(desc);
    if (offset == 0) {
        return 0;
    }
    if (offset > desc->coro_size ||
        desc->stack_size > desc->coro_size - offset) {
        return -1;
    }

    platform->stack_low  = (void*)(base + offset);
    platform->stack_size = desc->stack_size;
    return 0;
}

static int _coro_stack_limit_valid(
    const platform_coro_t* platform,
    const void*            limit) {
    uintptr_t low;
    uintptr_t high;
    uintptr_t value;
    size_t    alignment;

    if (limit == NULL) {
        return 1;
    }
    alignment = atomic_load_explicit(
        &_coro_stack_limit_alignment,
        memory_order_acquire);
    if (platform == NULL || platform->stack_low == NULL ||
        platform->stack_size == 0 || alignment == 0) {
        return 0;
    }
    low = (uintptr_t)platform->stack_low;
    if (platform->stack_size > (size_t)(UINTPTR_MAX - low)) {
        return 0;
    }
    high  = low + platform->stack_size;
    value = (uintptr_t)limit;
    return low < value && value < high && value % alignment == 0;
}

static int _coro_slot_init_cb(void* ptr, size_t size, void* ud) {
    const mco_desc* desc = (const mco_desc*)ud;
    platform_coro_t platform;
    size_t          alignment;

    if (_coro_platform(ptr, size, desc, &platform) != 0) {
        return -1;
    }
    if (platform.stack_low != NULL) {
        alignment = platform_vmem_page_size();
        if (alignment == 0) {
            return -1;
        }
        atomic_store_explicit(
            &_coro_stack_limit_alignment,
            alignment,
            memory_order_release);
    }
    return platform_coro_prepare_initial_layout(&platform);
}

mco_result coro_create(mco_coro** out, mco_desc* desc) {
    platform_coro_t platform;
    mco_coro*       co;
    void*           saved_stack_limit;
    mco_result      result;

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
    if (_coro_platform(co, desc->coro_size, desc, &platform) != 0) {
        desc->dealloc_cb(co, desc->coro_size, desc->allocator_data);
        return MCO_MAKE_CONTEXT_ERROR;
    }
    saved_stack_limit = mco_get_stack_limit(co);
    if (!_coro_stack_limit_valid(&platform, saved_stack_limit)) {
        abort();
    }
    result = mco_init(co, desc);
    if (result != MCO_SUCCESS) {
        desc->dealloc_cb(co, desc->coro_size, desc->allocator_data);
        *out = NULL;
        return result;
    }
    if (saved_stack_limit == NULL && platform.stack_low != NULL) {
        saved_stack_limit = platform_coro_initial_stack_limit(&platform);
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
