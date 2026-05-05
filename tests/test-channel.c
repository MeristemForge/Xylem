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
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "assert.h"

#include <stdatomic.h>
#include <stdio.h>

#define SAFETY_TIMEOUT_MS 5000

static xylem_runtime_opts_t _rt_opts = { .workers = 4 };

static void _safety_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)ud;
    sched_timer_destroy(timer);
    xylem_runtime_shutdown();
    ASSERT(0 && "test timed out");
}

static void _start_safety_timer(void) {
    sched_timer_t* t = sched_timer_create(runtime_get_scheduler());
    sched_timer_start(t, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);
}

#define CH_SENDERS  20
#define CH_MESSAGES 10

typedef struct {
    xylem_channel_t* ch;
    atomic_int       recv_count;
    int              tested;
} _ch_ctx_t;

static void _ch_sender(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    for (int i = 0; i < CH_MESSAGES; i++) {
        static int val = 1;
        xylem_channel_send(ctx->ch, &val);
    }
}

static void _ch_receiver(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    int total = CH_SENDERS * CH_MESSAGES;
    for (int i = 0; i < total; i++) {
        void* msg = xylem_channel_recv(ctx->ch);
        if (!msg) {
            break;
        }
        atomic_fetch_add(&ctx->recv_count, 1);
    }
    ASSERT(atomic_load(&ctx->recv_count) == total);
    ctx->tested = 1;
    xylem_runtime_shutdown();
}

static void _test_ch_main(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    _start_safety_timer();
    ctx->ch = xylem_channel_create();
    xylem_runtime_spawn(_ch_receiver, ctx);
    for (int i = 0; i < CH_SENDERS; i++) {
        xylem_runtime_spawn(_ch_sender, ctx);
    }
}

static void test_channel_concurrent(void) {
    fprintf(stderr, "=== test_channel_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _ch_ctx_t ctx = {0};
        xylem_runtime_run(_test_ch_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
        xylem_channel_destroy(ctx.ch);
    }
}

int main(void) {
    test_channel_concurrent();
    return 0;
}
