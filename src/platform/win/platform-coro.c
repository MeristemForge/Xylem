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

#include <stdint.h>

typedef struct {
    uint8_t* slot_low;
    uint8_t* stack_low;
    uint8_t* stack_high;
    uint8_t* guard_low;
    size_t   slot_size;
    size_t   metadata_size;
    size_t   stack_size;
    size_t   page_size;
    int      external;
} _coro_layout_t;

static int _coro_validate_slot(
    void*           slot,
    size_t          slot_size,
    _coro_layout_t* layout) {
    size_t    page_size;
    uintptr_t slot_low;

    if (slot == NULL || slot_size == 0 || layout == NULL) {
        return -1;
    }
    page_size = platform_vmem_page_size();
    slot_low  = (uintptr_t)slot;
    if (page_size == 0 || slot_low % page_size != 0 ||
        slot_size % page_size != 0 ||
        slot_size > (size_t)(UINTPTR_MAX - slot_low)) {
        return -1;
    }
    layout->slot_low  = (uint8_t*)slot_low;
    layout->slot_size = slot_size;
    layout->page_size = page_size;
    layout->external  = 0;
    return 0;
}

static int _coro_validate_stack_layout(
    size_t          stack_offset,
    size_t          stack_size,
    _coro_layout_t* layout) {
    size_t page_size = layout->page_size;

    if (stack_offset == 0 && stack_size == 0) {
        layout->external = 1;
        return 0;
    }
    if (stack_offset == 0 || stack_size == 0) {
        return -1;
    }
    if (stack_offset % page_size != 0 || stack_size % page_size != 0 ||
        page_size > SIZE_MAX / 2 || stack_size < page_size * 2 ||
        stack_offset > layout->slot_size ||
        stack_size > layout->slot_size - stack_offset) {
        return -1;
    }

    layout->metadata_size = stack_offset;
    layout->stack_low     = layout->slot_low + stack_offset;
    layout->stack_high    = layout->stack_low + stack_size;
    layout->stack_size    = stack_size;
    layout->guard_low     = layout->stack_high - page_size * 2;
    return 0;
}

static int _coro_validate(
    void*           slot,
    size_t          slot_size,
    size_t          stack_offset,
    size_t          stack_size,
    _coro_layout_t* layout) {
    if (_coro_validate_slot(slot, slot_size, layout) != 0) {
        return -1;
    }
    return _coro_validate_stack_layout(stack_offset, stack_size, layout);
}

static int _coro_commit_initial_stack(const _coro_layout_t* layout) {
    size_t commit_size = layout->page_size * 2;

    if (platform_vmem_commit(layout->guard_low, commit_size) != 0) {
        return -1;
    }
    if (platform_vmem_guard(layout->guard_low, layout->page_size) != 0) {
        return -1;
    }
    return 0;
}

int platform_coro_prepare_slot(
    void*  slot,
    size_t slot_size,
    size_t stack_offset,
    size_t stack_size) {
    _coro_layout_t layout;

    if (_coro_validate(
            slot,
            slot_size,
            stack_offset,
            stack_size,
            &layout) != 0) {
        return -1;
    }
    if (layout.external) {
        return platform_vmem_commit(layout.slot_low, layout.slot_size);
    }
    if (platform_vmem_commit(layout.slot_low, layout.metadata_size) != 0) {
        return -1;
    }
    if (_coro_commit_initial_stack(&layout) != 0) {
        (void)platform_vmem_decommit(layout.slot_low, layout.slot_size);
        return -1;
    }
    return 0;
}

void* platform_coro_initial_stack_limit(
    void*  stack_base,
    size_t stack_size) {
    size_t    page_size = platform_vmem_page_size();
    uintptr_t stack_low = (uintptr_t)stack_base;

    if (stack_base == NULL || page_size == 0 ||
        stack_low % page_size != 0 || stack_size % page_size != 0 ||
        page_size > SIZE_MAX / 2 || stack_size < page_size * 2 ||
        stack_size > (size_t)(UINTPTR_MAX - stack_low)) {
        return NULL;
    }
    return (uint8_t*)stack_base + stack_size - page_size;
}
