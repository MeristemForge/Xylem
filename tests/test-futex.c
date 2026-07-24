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

#include "xylem/xylem-threads.h"

#include "assert.h"
#include "platform/platform-futex.h"

#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    _Atomic uint32_t word;
    _Atomic bool     started;
} _wait_ctx_t;

static int _wait_worker(void* arg) {
    _wait_ctx_t* ctx = (_wait_ctx_t*)arg;
    atomic_store(&ctx->started, true);
    platform_futex_wait(&ctx->word, 0);
    return 0;
}

static void test_value_changed(void) {
    _Atomic uint32_t word;
    atomic_init(&word, 1);
    ASSERT(platform_futex_timedwait(&word, 0, 0));
}

static void test_zero_timeout(void) {
    _Atomic uint32_t word;
    atomic_init(&word, 0);
    ASSERT(!platform_futex_timedwait(&word, 0, 0));
}

static void test_signal(void) {
    _wait_ctx_t ctx;
    atomic_init(&ctx.word, 0);
    atomic_init(&ctx.started, false);

    thrd_t thrd;
    ASSERT(thrd_create(&thrd, _wait_worker, &ctx) == thrd_success);
    while (!atomic_load(&ctx.started)) {
        thrd_yield();
    }
    atomic_store(&ctx.word, 1);
    platform_futex_signal(&ctx.word);
    ASSERT(thrd_join(thrd, NULL) == thrd_success);
}

int main(void) {
    test_value_changed();
    test_zero_timeout();
    test_signal();
    return 0;
}
