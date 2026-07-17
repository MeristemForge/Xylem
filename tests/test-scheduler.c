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

#include "xylem.h"
#include "assert.h"

#include "runtime/runtime.h"
#include "runtime/scheduler-test.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>

typedef struct {
    scheduler_test_park_hook_t hook;
    atomic_int                 hook_calls;
    int                        resumed;
} _sched_test_ctx_t;

static void _park_hook(void* arg) {
    _sched_test_ctx_t* ctx = (_sched_test_ctx_t*)arg;
    atomic_fetch_add(&ctx->hook_calls, 1);
}

static bool _decline_commit(mco_coro* co, void* arg) {
    (void)co;
    (void)arg;
    return false;
}

static void _park_coro(void* arg) {
    _sched_test_ctx_t* ctx = (_sched_test_ctx_t*)arg;
    ctx->hook.target = mco_running();
    scheduler_test_set_park_hook(runtime_get_scheduler(), &ctx->hook);
    scheduler_park(runtime_get_scheduler(), _decline_commit, NULL);
    ctx->resumed = 1;
}

static void _test_main(void* arg) {
    _sched_test_ctx_t* ctx = (_sched_test_ctx_t*)arg;
    ctx->hook.fn  = _park_hook;
    ctx->hook.arg = ctx;
    xylem_spawn(_park_coro, ctx);
}

static void test_declined_park_checkpoint(void) {
    _sched_test_ctx_t ctx = {0};
    xylem_opts_t      opts = {.workers = 1};

    xylem_run(_test_main, &ctx, &opts);

    ASSERT(atomic_load(&ctx.hook_calls) == 1);
    ASSERT(ctx.resumed == 1);
}

int main(void) {
    test_declined_park_checkpoint();
    return 0;
}
