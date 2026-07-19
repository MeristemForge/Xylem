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

#include "platform/platform-coro.h"

#include "platform/platform-vmem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>

typedef struct {
    uint8_t* slot_low;
    uint8_t* stack_low;
    uint8_t* stack_high;
    uint8_t* guard_low;
    void*    initial_stack_limit;
    size_t   slot_size;
    size_t   metadata_size;
    size_t   stack_size;
    size_t   page_size;
    int      external;
} _coro_layout_t;

static int _coro_validate_slot(
    const platform_coro_t* coro,
    _coro_layout_t* layout) {
    size_t    page_size;
    uintptr_t slot_low;

    if (coro == NULL || layout == NULL || coro->ptr == NULL ||
        coro->size == 0) {
        return -1;
    }
    page_size = platform_vmem_page_size();
    slot_low  = (uintptr_t)coro->ptr;
    if (page_size == 0 || slot_low % page_size != 0 ||
        coro->size % page_size != 0 ||
        coro->size > (size_t)(UINTPTR_MAX - slot_low)) {
        return -1;
    }
    layout->slot_low  = (uint8_t*)slot_low;
    layout->slot_size = coro->size;
    layout->page_size = page_size;
    layout->external  = 0;
    return 0;
}

static int _coro_validate_stack_layout(
    const platform_coro_t* coro,
    _coro_layout_t*        layout) {
    size_t    page_size = layout->page_size;
    size_t    prefix_size;
    uintptr_t slot_low  = (uintptr_t)layout->slot_low;
    uintptr_t slot_high = slot_low + layout->slot_size;
    uintptr_t stack_low;
    uintptr_t stack_high;

    if (coro->stack_low == NULL && coro->stack_size == 0) {
        layout->external = 1;
        return 0;
    }
    if (coro->stack_low == NULL || coro->stack_size == 0) {
        return -1;
    }

    stack_low = (uintptr_t)coro->stack_low;
    if (stack_low % page_size != 0 || coro->stack_size % page_size != 0 ||
        page_size > SIZE_MAX / 2 || coro->stack_size < page_size * 2 ||
        coro->stack_size > (size_t)(UINTPTR_MAX - stack_low)) {
        return -1;
    }
    stack_high = stack_low + coro->stack_size;
    if (stack_low < slot_low || stack_high > slot_high) {
        return -1;
    }
    prefix_size = (size_t)(stack_low - slot_low);
    if (prefix_size <= page_size) {
        return -1;
    }
    layout->metadata_size = prefix_size - page_size;
    if (layout->metadata_size == 0 || layout->metadata_size % page_size != 0) {
        return -1;
    }

    layout->stack_low           = (uint8_t*)stack_low;
    layout->stack_high          = (uint8_t*)stack_high;
    layout->stack_size          = coro->stack_size;
    layout->guard_low           = layout->stack_high - page_size * 2;
    layout->initial_stack_limit = layout->stack_high - page_size;
    return 0;
}

static int _coro_validate(const platform_coro_t* coro, _coro_layout_t* layout) {
    if (_coro_validate_slot(coro, layout) != 0) {
        return -1;
    }
    return _coro_validate_stack_layout(coro, layout);
}

static int _coro_commit_initial_stack(const _coro_layout_t* layout) {
    DWORD  previous_protection;
    size_t commit_size = layout->page_size * 2;
    void*  committed;

    /* VirtualAlloc may touch the range before returning to its caller. */
    VMEM_ASAN_UNPOISON(layout->guard_low, commit_size);
    committed = VirtualAlloc(
        layout->guard_low,
        commit_size,
        MEM_COMMIT,
        PAGE_READWRITE);
    if (committed != layout->guard_low) {
        VMEM_ASAN_POISON(layout->guard_low, commit_size);
        return -1;
    }
    if (!VirtualProtect(
            layout->guard_low,
            layout->page_size,
            PAGE_READWRITE | PAGE_GUARD,
            &previous_protection)) {
        return -1;
    }
    return 0;
}

int platform_coro_init(const platform_coro_t* coro) {
    _coro_layout_t layout;

    if (_coro_validate_slot(coro, &layout) != 0) {
        return -1;
    }
    if (_coro_validate_stack_layout(coro, &layout) != 0) {
        (void)platform_vmem_decommit(layout.slot_low, layout.slot_size);
        return -1;
    }
    if (platform_vmem_decommit(layout.slot_low, layout.slot_size) != 0) {
        return -1;
    }
    if (layout.external) {
        if (platform_vmem_commit(layout.slot_low, layout.slot_size) == 0) {
            return 0;
        }
        (void)platform_vmem_decommit(layout.slot_low, layout.slot_size);
        return -1;
    }
    if (platform_vmem_commit(layout.slot_low, layout.metadata_size) != 0 ||
        _coro_commit_initial_stack(&layout) != 0) {
        (void)platform_vmem_decommit(layout.slot_low, layout.slot_size);
        return -1;
    }
    return 0;
}

int platform_coro_reset(
    const platform_coro_t* coro,
    void*                  current_stack_limit) {
    _coro_layout_t layout;

    if (_coro_validate(coro, &layout) != 0) {
        return -1;
    }
    if (layout.external) {
        return 0;
    }
    if (current_stack_limit == layout.initial_stack_limit) {
        return 0;
    }
    if (platform_vmem_decommit(layout.stack_low, layout.stack_size) != 0) {
        return -1;
    }
    if (_coro_commit_initial_stack(&layout) != 0) {
        (void)platform_vmem_decommit(layout.stack_low, layout.stack_size);
        return -1;
    }
    return 0;
}

void* platform_coro_initial_stack_limit(const platform_coro_t* coro) {
    _coro_layout_t layout;

    if (_coro_validate(coro, &layout) != 0 || layout.external) {
        return NULL;
    }
    return layout.initial_stack_limit;
}
