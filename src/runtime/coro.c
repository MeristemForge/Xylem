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

#define CORO_STACK_PLAN_UNKNOWN  ((size_t)0)
#define CORO_STACK_PLAN_EXTERNAL SIZE_MAX
#define CORO_ALLOC_CTX_READY      ((size_t)0xc07a110cU)

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

static void* _coro_alloc_cb(size_t size, void* allocator_data) {
    coro_alloc_ctx_t* ctx = (coro_alloc_ctx_t*)allocator_data;

    if (ctx == NULL || ctx->alloc_cb == NULL) {
        return NULL;
    }
    return ctx->alloc_cb(size, ctx->allocator_data);
}

static void _coro_dealloc_cb(
    void* ptr,
    size_t size,
    void* allocator_data) {
    coro_alloc_ctx_t* ctx = (coro_alloc_ctx_t*)allocator_data;

    if (ctx == NULL || ctx->dealloc_cb == NULL) {
        return;
    }
    ctx->dealloc_cb(ptr, size, ctx->allocator_data);
}

static int _coro_desc_matches_ctx(
    const mco_desc* desc,
    const coro_alloc_ctx_t* ctx) {
    const mco_desc* layout;

    if (desc == NULL || ctx == NULL || ctx->state != CORO_ALLOC_CTX_READY) {
        return 0;
    }
    layout = &ctx->layout;
    return desc->func == layout->func &&
           desc->alloc_cb == _coro_alloc_cb &&
           desc->dealloc_cb == _coro_dealloc_cb &&
           desc->allocator_data == ctx &&
           desc->storage_size == layout->storage_size &&
           desc->coro_size == layout->coro_size &&
           desc->stack_size == layout->stack_size &&
           desc->stack_offset == layout->stack_offset &&
           desc->stack_alignment == layout->stack_alignment;
}

static int _coro_publish_stack_plan(
    coro_alloc_ctx_t* ctx,
    const platform_coro_t* platform) {
    size_t    plan;
    size_t    expected = CORO_STACK_PLAN_UNKNOWN;
    uintptr_t base;
    uintptr_t limit;
    void*     initial_limit;

    initial_limit = platform_coro_initial_stack_limit(platform);
    if (platform->stack_low == NULL) {
        if (initial_limit != NULL) {
            return -1;
        }
        plan = CORO_STACK_PLAN_EXTERNAL;
    } else {
        base = (uintptr_t)platform->ptr;
        limit = (uintptr_t)initial_limit;
        if (initial_limit == NULL || limit <= base ||
            limit - base >= platform->size) {
            return -1;
        }
        plan = (size_t)(limit - base);
        if (plan == CORO_STACK_PLAN_EXTERNAL) {
            return -1;
        }
    }

    if (atomic_compare_exchange_strong(&ctx->stack_plan, &expected, plan)) {
        return 0;
    }
    return expected == plan ? 0 : -1;
}

static int _coro_slot_init_cb(void* ptr, size_t size, void* ud) {
    coro_alloc_ctx_t* ctx = (coro_alloc_ctx_t*)ud;
    platform_coro_t platform;

    if (ctx == NULL || !_coro_desc_matches_ctx(ctx->desc, ctx) ||
        _coro_platform(ptr, size, &ctx->layout, &platform) != 0) {
        return -1;
    }
    if (platform_coro_init(&platform) != 0) {
        return -1;
    }
    return _coro_publish_stack_plan(ctx, &platform);
}

static int _coro_slot_reset_cb(void* ptr, size_t size, void* ud) {
    coro_alloc_ctx_t* ctx = (coro_alloc_ctx_t*)ud;
    size_t            plan;
    uintptr_t         base;
    platform_coro_t   platform;

    if (ctx == NULL || ctx->state != CORO_ALLOC_CTX_READY || ptr == NULL ||
        size == 0) {
        return -1;
    }
    plan = atomic_load(&ctx->stack_plan);
    if (plan == CORO_STACK_PLAN_EXTERNAL) {
        return 0;
    }
    base = (uintptr_t)ptr;
    if (plan != CORO_STACK_PLAN_UNKNOWN && plan < size &&
        plan <= (size_t)(UINTPTR_MAX - base) &&
        mco_get_stack_limit((const mco_coro*)ptr) == (void*)(base + plan)) {
        return 0;
    }

    if (!_coro_desc_matches_ctx(ctx->desc, ctx) ||
        _coro_platform(ptr, size, &ctx->layout, &platform) != 0) {
        return -1;
    }
    return platform_coro_reset(
        &platform,
        mco_get_stack_limit((const mco_coro*)ptr));
}

static coro_alloc_ctx_t* _coro_get_alloc_ctx(const mco_desc* desc) {
    coro_alloc_ctx_t* ctx;

    if (desc == NULL || desc->alloc_cb != _coro_alloc_cb ||
        desc->dealloc_cb != _coro_dealloc_cb ||
        desc->allocator_data == NULL) {
        return NULL;
    }
    ctx = (coro_alloc_ctx_t*)desc->allocator_data;
    if (ctx->state != CORO_ALLOC_CTX_READY || ctx->desc == NULL) {
        return NULL;
    }
    return ctx;
}

static int _coro_apply_stack_plan(
    mco_coro* co,
    const mco_desc* desc,
    const coro_alloc_ctx_t* ctx) {
    size_t plan = atomic_load(&ctx->stack_plan);
    size_t stack_offset;

    if (plan == CORO_STACK_PLAN_UNKNOWN) {
        return -1;
    }
    stack_offset = ctx->layout.stack_offset;
    if (plan == CORO_STACK_PLAN_EXTERNAL) {
        if (stack_offset != 0) {
            return -1;
        }
        mco_set_stack_limit(co, NULL);
        return 0;
    }
    if (stack_offset == 0 || plan < stack_offset ||
        plan - stack_offset >= desc->stack_size || plan >= desc->coro_size) {
        return -1;
    }
    mco_set_stack_limit(co, (void*)((uintptr_t)co + plan));
    return 0;
}

int coro_alloc_ctx_init(coro_alloc_ctx_t* ctx, mco_desc* desc) {
    if (ctx == NULL || desc == NULL || desc->alloc_cb == NULL ||
        desc->dealloc_cb == NULL || ctx->state != 0 ||
        desc->alloc_cb == _coro_alloc_cb ||
        desc->dealloc_cb == _coro_dealloc_cb) {
        return -1;
    }

    ctx->desc           = desc;
    ctx->layout         = *desc;
    ctx->alloc_cb       = desc->alloc_cb;
    ctx->dealloc_cb     = desc->dealloc_cb;
    ctx->allocator_data = desc->allocator_data;
    atomic_init(&ctx->stack_plan, CORO_STACK_PLAN_UNKNOWN);

    desc->alloc_cb       = _coro_alloc_cb;
    desc->dealloc_cb     = _coro_dealloc_cb;
    desc->allocator_data = ctx;
    ctx->state           = CORO_ALLOC_CTX_READY;
    return 0;
}

void coro_alloc_ctx_deinit(coro_alloc_ctx_t* ctx) {
    if (ctx == NULL || ctx->state != CORO_ALLOC_CTX_READY ||
        ctx->desc == NULL) {
        return;
    }

    ctx->desc->alloc_cb       = ctx->alloc_cb;
    ctx->desc->dealloc_cb     = ctx->dealloc_cb;
    ctx->desc->allocator_data = ctx->allocator_data;
    ctx->desc           = NULL;
    ctx->layout         = (mco_desc){0};
    ctx->alloc_cb       = NULL;
    ctx->dealloc_cb     = NULL;
    ctx->allocator_data = NULL;
    atomic_store(&ctx->stack_plan, CORO_STACK_PLAN_UNKNOWN);
    ctx->state          = 0;
}

mco_result coro_create(mco_coro** out, mco_desc* desc) {
    coro_alloc_ctx_t* ctx = _coro_get_alloc_ctx(desc);
    mco_result        result;

    if (ctx == NULL) {
        if (desc != NULL &&
            (desc->alloc_cb == _coro_alloc_cb ||
             desc->dealloc_cb == _coro_dealloc_cb)) {
            if (out != NULL) {
                *out = NULL;
            }
            return MCO_INVALID_ARGUMENTS;
        }
        return mco_create(out, desc);
    }
    if (!_coro_desc_matches_ctx(desc, ctx)) {
        if (out != NULL) {
            *out = NULL;
        }
        return MCO_INVALID_ARGUMENTS;
    }

    result = mco_create(out, desc);
    if (result != MCO_SUCCESS) {
        return result;
    }
    if (_coro_apply_stack_plan(*out, desc, ctx) != 0) {
        mco_coro* co = *out;

        *out = NULL;
        (void)mco_destroy(co);
        return MCO_MAKE_CONTEXT_ERROR;
    }
    return MCO_SUCCESS;
}

mco_result coro_destroy(mco_coro* co) {
    return mco_destroy(co);
}

copool_slot_ops_t coro_get_slot_ops(coro_alloc_ctx_t* ctx) {
    copool_slot_ops_t ops = {
        .init  = _coro_slot_init_cb,
        .reset = _coro_slot_reset_cb,
        .ud    = ctx,
    };

    return ops;
}
