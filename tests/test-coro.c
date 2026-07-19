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

#include "assert.h"

#include "runtime/minicoro/minicoro.h"

#include <stdint.h>

static void _empty_entry(mco_coro* co) {
    (void)co;
}

static void test_stack_offset(void) {
    mco_desc  desc = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_coro* co   = NULL;
    size_t    offset;

    ASSERT(mco_create(&co, &desc) == MCO_SUCCESS);
    offset = mco_desc_stack_offset(&desc);
    if (offset != 0) {
        ASSERT((uint8_t*)co->stack_base == (uint8_t*)co + offset);
        ASSERT(co->stack_size == desc.stack_size);
    }
    ASSERT(mco_destroy(co) == MCO_SUCCESS);
}

static void test_stack_limit(void) {
    mco_desc  desc = mco_desc_init(_empty_entry, 128U * 1024U);
    mco_coro* co   = NULL;
    void*     original;

    ASSERT(mco_create(&co, &desc) == MCO_SUCCESS);
    original = mco_get_stack_limit(co);
    if (original != NULL) {
        void* replacement = (uint8_t*)original + 16;

        mco_set_stack_limit(co, replacement);
        ASSERT(mco_get_stack_limit(co) == replacement);
        mco_set_stack_limit(co, original);
        ASSERT(mco_get_stack_limit(co) == original);
    } else {
        mco_set_stack_limit(co, NULL);
        ASSERT(mco_get_stack_limit(co) == NULL);
    }
    ASSERT(mco_destroy(co) == MCO_SUCCESS);
}

int main(void) {
    test_stack_offset();
    test_stack_limit();
    return 0;
}
