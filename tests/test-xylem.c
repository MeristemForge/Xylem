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
#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdio.h>

#define SAFETY_TIMEOUT_MS 5000
#define SPAWN_MANY_COUNT  1000
#define SLEEP_ORDER_COUNT 4
#define SUBMIT_CONC_COUNT 20

#define STKGROW_FRAME_BYTES   (8 * 1024)
#define STKGROW_BASIC_DEPTH   4
#define STKGROW_RECURSE_DEPTH 8
#define STKGROW_CONC_COUNT    32
#define STKGROW_CONC_DEPTH    6
#define STKGROW_REUSE_COUNT   50
#define STKGROW_REUSE_DEPTH   3

static xylem_opts_t _rt_opts = { .workers = 4 };

static void _timeout_coro(void* arg) {
    (void)arg;
    xylem_sleep(SAFETY_TIMEOUT_MS);
    xylem_shutdown();
    ASSERT(0 && "test timed out");
}

static void _start_safety_timer(void) {
    xylem_spawn(_timeout_coro, NULL);
}

static void _cycle_main(void* arg) {
    int* val = (int*)arg;
    _start_safety_timer();
    (*val)++;
    xylem_shutdown();
}

static void test_start_stop_cycle(void) {
    fprintf(stderr, "=== test_start_stop_cycle\n");
    int val = 0;
    for (int i = 0; i < 5; i++) {
        xylem_run(_cycle_main, &val, &_rt_opts);
    }
    ASSERT(val == 5);
}

typedef struct {
    int tested;
} _stop_ctx_t;

static void _stop_spawned(void* arg) {
    _stop_ctx_t* ctx = (_stop_ctx_t*)arg;
    ctx->tested = 1;
    xylem_shutdown();
}

static void _stop_main(void* arg) {
    _start_safety_timer();
    xylem_spawn(_stop_spawned, arg);
}

static void test_stop_from_spawned(void) {
    fprintf(stderr, "=== test_stop_from_spawned\n");
    _stop_ctx_t ctx = {0};
    xylem_run(_stop_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

typedef struct {
    atomic_int count;
    int        target;
} _ext_ctx_t;

static void _ext_coro(void* arg) {
    _ext_ctx_t* ctx = (_ext_ctx_t*)arg;
    if (atomic_fetch_add(&ctx->count, 1) == ctx->target - 1) {
        xylem_shutdown();
    }
}

static int _ext_thread_fn(void* arg) {
    _ext_ctx_t* ctx = (_ext_ctx_t*)arg;
    for (int i = 0; i < ctx->target; i++) {
        xylem_spawn(_ext_coro, ctx);
    }
    return 0;
}

static void _ext_main(void* arg) {
    _ext_ctx_t* ctx = (_ext_ctx_t*)arg;
    _start_safety_timer();
    thrd_t th;
    thrd_create(&th, _ext_thread_fn, ctx);
    thrd_detach(th);
}

static void test_spawn_from_external_thread(void) {
    fprintf(stderr, "=== test_spawn_from_external_thread\n");
    _ext_ctx_t ctx = { .target = 50 };
    atomic_init(&ctx.count, 0);
    xylem_run(_ext_main, &ctx, &_rt_opts);
    ASSERT(atomic_load(&ctx.count) == ctx.target);
}

typedef struct {
    atomic_int depth_reached;
} _nest_ctx_t;

static void _nest_child(void* arg) {
    _nest_ctx_t* ctx = (_nest_ctx_t*)arg;
    atomic_store(&ctx->depth_reached, 2);
    xylem_shutdown();
}

static void _nest_parent(void* arg) {
    _nest_ctx_t* ctx = (_nest_ctx_t*)arg;
    atomic_store(&ctx->depth_reached, 1);
    xylem_spawn(_nest_child, ctx);
}

static void _nest_main(void* arg) {
    _start_safety_timer();
    xylem_spawn(_nest_parent, arg);
}

static void test_spawn_nested(void) {
    fprintf(stderr, "=== test_spawn_nested\n");
    _nest_ctx_t ctx = {0};
    atomic_init(&ctx.depth_reached, 0);
    xylem_run(_nest_main, &ctx, &_rt_opts);
    ASSERT(atomic_load(&ctx.depth_reached) == 2);
}

typedef struct {
    atomic_int done;
} _many_ctx_t;

static void _many_coro(void* arg) {
    _many_ctx_t* ctx = (_many_ctx_t*)arg;
    if (atomic_fetch_add(&ctx->done, 1) == SPAWN_MANY_COUNT - 1) {
        xylem_shutdown();
    }
}

static void _many_main(void* arg) {
    _many_ctx_t* ctx = (_many_ctx_t*)arg;
    _start_safety_timer();
    for (int i = 0; i < SPAWN_MANY_COUNT; i++) {
        xylem_spawn(_many_coro, ctx);
    }
}

static void test_spawn_many(void) {
    fprintf(stderr, "=== test_spawn_many\n");
    _many_ctx_t ctx = {0};
    atomic_init(&ctx.done, 0);
    xylem_run(_many_main, &ctx, &_rt_opts);
    ASSERT(atomic_load(&ctx.done) == SPAWN_MANY_COUNT);
}

typedef struct {
    int        order[2];
    atomic_int idx;
} _sleep0_ctx_t;

static void _sleep0_first(void* arg) {
    _sleep0_ctx_t* ctx = (_sleep0_ctx_t*)arg;
    xylem_sleep(0);
    ctx->order[atomic_fetch_add(&ctx->idx, 1)] = 1;
    if (atomic_load(&ctx->idx) == 2) {
        xylem_shutdown();
    }
}

static void _sleep0_second(void* arg) {
    _sleep0_ctx_t* ctx = (_sleep0_ctx_t*)arg;
    ctx->order[atomic_fetch_add(&ctx->idx, 1)] = 2;
    if (atomic_load(&ctx->idx) == 2) {
        xylem_shutdown();
    }
}

static void _sleep0_main(void* arg) {
    _start_safety_timer();
    xylem_spawn(_sleep0_first, arg);
    xylem_spawn(_sleep0_second, arg);
}

static void test_sleep_zero(void) {
    fprintf(stderr, "=== test_sleep_zero\n");
    _sleep0_ctx_t ctx = {0};
    atomic_init(&ctx.idx, 0);
    xylem_run(_sleep0_main, &ctx, &_rt_opts);
    ASSERT(atomic_load(&ctx.idx) == 2);
    ASSERT((ctx.order[0] == 1 && ctx.order[1] == 2) ||
           (ctx.order[0] == 2 && ctx.order[1] == 1));
}

typedef struct {
    uint64_t   elapsed[SLEEP_ORDER_COUNT];
    atomic_int idx;
} _sleepord_ctx_t;

typedef struct {
    _sleepord_ctx_t* ctx;
    int              id;
} _sleepord_arg_t;

static void _sleepord_coro(void* arg) {
    _sleepord_arg_t* a  = (_sleepord_arg_t*)arg;
    uint64_t         ms = (uint64_t)((a->id + 1) * 30);
    uint64_t start = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    xylem_sleep(ms);
    uint64_t end = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    a->ctx->elapsed[a->id] = end - start;
    if (atomic_fetch_add(&a->ctx->idx, 1) + 1 == SLEEP_ORDER_COUNT) {
        xylem_shutdown();
    }
}

static _sleepord_arg_t _sleepord_args[SLEEP_ORDER_COUNT];

static void _sleepord_main(void* arg) {
    _sleepord_ctx_t* ctx = (_sleepord_ctx_t*)arg;
    _start_safety_timer();
    for (int i = 0; i < SLEEP_ORDER_COUNT; i++) {
        _sleepord_args[i].ctx = ctx;
        _sleepord_args[i].id  = i;
        xylem_spawn(_sleepord_coro, &_sleepord_args[i]);
    }
}

static void test_sleep_ordering(void) {
    fprintf(stderr, "=== test_sleep_ordering\n");
    _sleepord_ctx_t ctx = {0};
    atomic_init(&ctx.idx, 0);
    xylem_run(_sleepord_main, &ctx, &_rt_opts);
    ASSERT(atomic_load(&ctx.idx) == SLEEP_ORDER_COUNT);
    for (int i = 0; i < SLEEP_ORDER_COUNT; i++) {
        uint64_t requested = (uint64_t)((i + 1) * 30);
        ASSERT(ctx.elapsed[i] + 2 >= requested);
    }
}

typedef struct {
    int input;
    int output;
    int tested;
} _submit_ctx_t;

static void _submit_blocking(void* arg) {
    _submit_ctx_t* ctx = (_submit_ctx_t*)arg;
    ctx->output = ctx->input * 2;
}

static void _submit_coro(void* arg) {
    _submit_ctx_t* ctx = (_submit_ctx_t*)arg;
    int            rc  = xylem_await(_submit_blocking, ctx);
    ASSERT(rc == 0);
    ASSERT(ctx->output == 84);
    ctx->tested = 1;
    xylem_shutdown();
}

static void _submit_main(void* arg) {
    _start_safety_timer();
    xylem_spawn(_submit_coro, arg);
}

static void test_submit_basic(void) {
    fprintf(stderr, "=== test_submit_basic\n");
    _submit_ctx_t ctx = { .input = 42, .output = 0, .tested = 0 };
    xylem_run(_submit_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

typedef struct {
    atomic_int done;
    int        tested;
} _submit_conc_ctx_t;

static void _submit_conc_blocking(void* arg) {
    (void)arg;
    volatile int sum = 0;
    for (int i = 0; i < 10000; i++) {
        sum += i;
    }
    (void)sum;
}

static void _submit_conc_coro(void* arg) {
    _submit_conc_ctx_t* ctx = (_submit_conc_ctx_t*)arg;
    int                 rc  = xylem_await(_submit_conc_blocking, NULL);
    ASSERT(rc == 0);
    if (atomic_fetch_add(&ctx->done, 1) == SUBMIT_CONC_COUNT - 1) {
        ctx->tested = 1;
        xylem_shutdown();
    }
}

static void _submit_conc_main(void* arg) {
    _submit_conc_ctx_t* ctx = (_submit_conc_ctx_t*)arg;
    _start_safety_timer();
    for (int i = 0; i < SUBMIT_CONC_COUNT; i++) {
        xylem_spawn(_submit_conc_coro, ctx);
    }
}

static void test_submit_concurrent(void) {
    fprintf(stderr, "=== test_submit_concurrent\n");
    _submit_conc_ctx_t ctx = {0};
    atomic_init(&ctx.done, 0);
    xylem_run(_submit_conc_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
    ASSERT(atomic_load(&ctx.done) == SUBMIT_CONC_COUNT);
}

typedef struct {
    int        values[4];
    atomic_int done;
    int        tested;
} _submit_res_ctx_t;

typedef struct {
    _submit_res_ctx_t* ctx;
    int                idx;
} _submit_res_arg_t;

static void _submit_res_blocking(void* arg) {
    _submit_res_arg_t* a = (_submit_res_arg_t*)arg;
    a->ctx->values[a->idx] = (a->idx + 1) * 10;
}

static void _submit_res_coro(void* arg) {
    _submit_res_arg_t* a  = (_submit_res_arg_t*)arg;
    int                rc = xylem_await(_submit_res_blocking, a);
    ASSERT(rc == 0);
    if (atomic_fetch_add(&a->ctx->done, 1) == 3) {
        a->ctx->tested = 1;
        xylem_shutdown();
    }
}

static _submit_res_arg_t _submit_res_args[4];

static void _submit_res_main(void* arg) {
    _submit_res_ctx_t* ctx = (_submit_res_ctx_t*)arg;
    _start_safety_timer();
    for (int i = 0; i < 4; i++) {
        _submit_res_args[i].ctx = ctx;
        _submit_res_args[i].idx = i;
        xylem_spawn(_submit_res_coro, &_submit_res_args[i]);
    }
}

static void test_submit_result(void) {
    fprintf(stderr, "=== test_submit_result\n");
    _submit_res_ctx_t ctx = {0};
    atomic_init(&ctx.done, 0);
    xylem_run(_submit_res_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
    for (int i = 0; i < 4; i++) {
        ASSERT(ctx.values[i] == (i + 1) * 10);
    }
}

static uint64_t _stkgrow_recurse(int depth, uint8_t seed) {
    volatile uint8_t frame[STKGROW_FRAME_BYTES];
    for (size_t i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(seed + (uint8_t)depth + (uint8_t)(i & 0x3f));
    }

    uint64_t deeper = 0;
    if (depth > 0) {
        deeper = _stkgrow_recurse(depth - 1, seed);
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < sizeof(frame); i++) {
        uint8_t expect =
            (uint8_t)(seed + (uint8_t)depth + (uint8_t)(i & 0x3f));
        ASSERT(frame[i] == expect);
        sum += frame[i];
    }
    return sum + deeper;
}

typedef struct {
    int tested;
} _stkgrow_basic_ctx_t;

static void _stkgrow_basic_coro(void* arg) {
    _stkgrow_basic_ctx_t* ctx = (_stkgrow_basic_ctx_t*)arg;
    uint64_t sum = _stkgrow_recurse(STKGROW_BASIC_DEPTH, 0xa5);
    ASSERT(sum > 0);
    ctx->tested = 1;
    xylem_shutdown();
}

static void _stkgrow_basic_main(void* arg) {
    _start_safety_timer();
    xylem_spawn(_stkgrow_basic_coro, arg);
}

static void test_coro_stack_grow(void) {
    fprintf(stderr, "=== test_coro_stack_grow\n");
    _stkgrow_basic_ctx_t ctx = {0};
    xylem_run(_stkgrow_basic_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

typedef struct {
    int tested;
} _stkgrow_prog_ctx_t;

static void _stkgrow_prog_coro(void* arg) {
    _stkgrow_prog_ctx_t* ctx = (_stkgrow_prog_ctx_t*)arg;
    uint64_t sum = _stkgrow_recurse(STKGROW_RECURSE_DEPTH, 0x37);
    ASSERT(sum > 0);
    ctx->tested = 1;
    xylem_shutdown();
}

static void _stkgrow_prog_main(void* arg) {
    _start_safety_timer();
    xylem_spawn(_stkgrow_prog_coro, arg);
}

static void test_coro_stack_grow_deep_recursion(void) {
    fprintf(stderr, "=== test_coro_stack_grow_deep_recursion\n");
    _stkgrow_prog_ctx_t ctx = {0};
    xylem_run(_stkgrow_prog_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

typedef struct {
    atomic_int done;
    int        tested;
} _stkgrow_conc_ctx_t;

typedef struct {
    _stkgrow_conc_ctx_t* ctx;
    int                  id;
} _stkgrow_conc_arg_t;

static _stkgrow_conc_arg_t _stkgrow_conc_args[STKGROW_CONC_COUNT];

static void _stkgrow_conc_coro(void* arg) {
    _stkgrow_conc_arg_t* a = (_stkgrow_conc_arg_t*)arg;
    uint8_t seed = (uint8_t)(0x10 + a->id);

    uint64_t sum = _stkgrow_recurse(STKGROW_CONC_DEPTH, seed);
    ASSERT(sum > 0);

    if (atomic_fetch_add(&a->ctx->done, 1) == STKGROW_CONC_COUNT - 1) {
        a->ctx->tested = 1;
        xylem_shutdown();
    }
}

static void _stkgrow_conc_main(void* arg) {
    _stkgrow_conc_ctx_t* ctx = (_stkgrow_conc_ctx_t*)arg;
    _start_safety_timer();
    for (int i = 0; i < STKGROW_CONC_COUNT; i++) {
        _stkgrow_conc_args[i].ctx = ctx;
        _stkgrow_conc_args[i].id  = i;
        xylem_spawn(_stkgrow_conc_coro, &_stkgrow_conc_args[i]);
    }
}

static void test_coro_stack_grow_concurrent(void) {
    fprintf(stderr, "=== test_coro_stack_grow_concurrent\n");
    _stkgrow_conc_ctx_t ctx = {0};
    atomic_init(&ctx.done, 0);
    xylem_run(_stkgrow_conc_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
    ASSERT(atomic_load(&ctx.done) == STKGROW_CONC_COUNT);
}

typedef struct {
    int counter;
    int target;
    int tested;
} _stkgrow_reuse_ctx_t;

static void _stkgrow_reuse_coro(void* arg) {
    _stkgrow_reuse_ctx_t* ctx = (_stkgrow_reuse_ctx_t*)arg;

    uint8_t  seed = (uint8_t)(0x80 + (ctx->counter & 0x7f));
    uint64_t sum  = _stkgrow_recurse(STKGROW_REUSE_DEPTH, seed);
    ASSERT(sum > 0);

    if (++ctx->counter < ctx->target) {
        xylem_spawn(_stkgrow_reuse_coro, ctx);
    } else {
        ctx->tested = 1;
        xylem_shutdown();
    }
}

static void _stkgrow_reuse_main(void* arg) {
    _stkgrow_reuse_ctx_t* ctx = (_stkgrow_reuse_ctx_t*)arg;
    _start_safety_timer();
    xylem_spawn(_stkgrow_reuse_coro, ctx);
}

static void test_coro_stack_grow_pool_reuse(void) {
    fprintf(stderr, "=== test_coro_stack_grow_pool_reuse\n");
    _stkgrow_reuse_ctx_t ctx = { .counter = 0,
                                 .target  = STKGROW_REUSE_COUNT,
                                 .tested  = 0 };
    xylem_opts_t opts = { .workers = 1 };
    xylem_run(_stkgrow_reuse_main, &ctx, &opts);
    ASSERT(ctx.tested == 1);
    ASSERT(ctx.counter == STKGROW_REUSE_COUNT);
}

typedef struct {
    int tested;
} _stkgrow_large_ctx_t;

static void _stkgrow_large_coro(void* arg) {
    _stkgrow_large_ctx_t* ctx = (_stkgrow_large_ctx_t*)arg;
    volatile uint8_t frame[64 * 1024];
    for (size_t i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i ^ 0xab);
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < sizeof(frame); i++) {
        ASSERT(frame[i] == (uint8_t)(i ^ 0xab));
        sum += frame[i];
    }
    ASSERT(sum > 0);
    ctx->tested = 1;
    xylem_shutdown();
}

static void _stkgrow_large_main(void* arg) {
    _start_safety_timer();
    xylem_spawn(_stkgrow_large_coro, arg);
}

static void test_coro_stack_grow_large_frame(void) {
    fprintf(stderr, "=== test_coro_stack_grow_large_frame\n");
    _stkgrow_large_ctx_t ctx = {0};
    xylem_run(_stkgrow_large_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

int main(void) {
    test_start_stop_cycle();
    test_stop_from_spawned();
    test_spawn_from_external_thread();
    test_spawn_nested();
    test_spawn_many();
    test_sleep_zero();
    test_sleep_ordering();
    test_submit_basic();
    test_submit_concurrent();
    test_submit_result();
    test_coro_stack_grow();
    test_coro_stack_grow_deep_recursion();
    test_coro_stack_grow_concurrent();
    test_coro_stack_grow_pool_reuse();
    test_coro_stack_grow_large_frame();
    return 0;
}
