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

#include <stdint.h>

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

static int _coro_slot_init_cb(void* ptr, size_t size, void* ud) {
    platform_coro_t platform;

    if (_coro_platform(ptr, size, (const mco_desc*)ud, &platform) != 0) {
        return -1;
    }
    return platform_coro_init(&platform);
}

static int _coro_slot_reset_cb(void* ptr, size_t size, void* ud) {
    platform_coro_t platform;

    if (_coro_platform(ptr, size, (const mco_desc*)ud, &platform) != 0) {
        return -1;
    }
    return platform_coro_reset(
        &platform,
        mco_get_stack_limit((const mco_coro*)ptr));
}

mco_result coro_create(mco_coro** out, mco_desc* desc) {
    platform_coro_t platform;
    mco_result      result = mco_create(out, desc);

    if (result != MCO_SUCCESS) {
        return result;
    }
    if (_coro_platform(*out, desc->coro_size, desc, &platform) != 0) {
        mco_coro* co = *out;

        *out = NULL;
        (void)mco_destroy(co);
        return MCO_INVALID_ARGUMENTS;
    }

    mco_set_stack_limit(*out, platform_coro_initial_stack_limit(&platform));
    return MCO_SUCCESS;
}

mco_result coro_destroy(mco_coro* co) {
    return mco_destroy(co);
}

copool_slot_ops_t coro_get_slot_ops(const mco_desc* desc) {
    copool_slot_ops_t ops = {
        .init  = _coro_slot_init_cb,
        .reset = _coro_slot_reset_cb,
        .ud    = (void*)desc,
    };

    return ops;
}
