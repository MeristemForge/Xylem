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

static xylem_opts_t _rt_opts = { .workers = 4 };

static void _safety_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)ud;
    sched_timer_destroy(timer);
    xylem_shutdown();
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
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _test_ch_main(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    _start_safety_timer();
    ctx->ch = xylem_channel_create();
    xylem_spawn(_ch_receiver, ctx);
    for (int i = 0; i < CH_SENDERS; i++) {
        xylem_spawn(_ch_sender, ctx);
    }
}

static void test_channel_concurrent(void) {
    fprintf(stderr, "=== test_channel_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _ch_ctx_t ctx = {0};
        xylem_run(_test_ch_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
    }
}

/* --- recv_timeout coverage ----------------------------------------
 *
 * Teardown rule (library-wide, not channel-specific): every xylem_
 * runtime object embeds scheduler-owned resources -- tcp/udp/uds/
 * dtls conns hold an iowait_t, dtls also holds sched_timer_t's, and
 * a timed channel caches a deadline sched_timer_t. Such objects must
 * be closed/destroyed while the runtime is still alive; touching one
 * after xylem_run() returns is undefined (the scheduler that owns
 * those resources is already freed). These tests therefore destroy
 * the channel from inside the owning coroutine before xylem_shutdown.
 * (A plain channel that never armed a timer happens to be
 * destroyable afterwards, which is why test_channel_concurrent does
 * so, but the timed variants follow the general rule.)
 */

typedef struct {
    xylem_channel_t* ch;
    int              tested;
} _to_ctx_t;

/* An empty channel with an elapsed deadline must report a timeout
 * (NULL) rather than blocking forever. */
static void _to_basic_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 30;
    void* msg = xylem_channel_recv_timeout(ctx->ch, deadline);
    ASSERT(msg == NULL);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _to_basic_main(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    _start_safety_timer();
    ctx->ch = xylem_channel_create();
    xylem_spawn(_to_basic_coro, ctx);
}

static void test_channel_timeout_empty(void) {
    fprintf(stderr, "=== test_channel_timeout_empty\n");
    _to_ctx_t ctx = {0};
    xylem_run(_to_basic_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* A message that arrives before the deadline must be delivered, not
 * swallowed by the timeout. */
static int _to_payload = 7;

static void _to_sender_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    xylem_sleep(20);
    xylem_channel_send(ctx->ch, &_to_payload);
}

static void _to_recv_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 1000;
    void* msg = xylem_channel_recv_timeout(ctx->ch, deadline);
    ASSERT(msg == &_to_payload);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _to_deliver_main(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    _start_safety_timer();
    ctx->ch = xylem_channel_create();
    xylem_spawn(_to_recv_coro, ctx);
    xylem_spawn(_to_sender_coro, ctx);
}

static void test_channel_timeout_deliver(void) {
    fprintf(stderr, "=== test_channel_timeout_deliver\n");
    _to_ctx_t ctx = {0};
    xylem_run(_to_deliver_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* Stress the send/deadline overlap: drive send to land at roughly
 * the same instant as the deadline, many times. This exercises the
 * exact window where the "false timeout strands a delivered message"
 * race lived. Invariants checked here:
 *   - recv_timeout returns either the correct payload pointer or
 *     NULL, never garbage;
 *   - exactly one of message/timeout is observed per round;
 *   - no crash, no use-after-free, no leak (run under ASAN to make
 *     the timer-callback refcount fix observable).
 * Note: a legitimate late send (landing after the deadline) also
 * yields a timeout with the message left in the queue, freed at
 * destroy; that is correct and indistinguishable from the outside,
 * so this is a robustness stress test rather than a deterministic
 * regression. */
#define TO_RACE_ROUNDS 200

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    uint64_t         deadline;
    int              got_timeout;
    int              got_message;
} _race_ctx_t;

static void _race_sender_coro(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    /* Aim the send at the deadline itself so the send wakeup and the
     * deadline timer fire in the same window. */
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (ctx->deadline > now) {
        xylem_sleep(ctx->deadline - now);
    }
    xylem_channel_send(ctx->ch, &ctx->payload);
}

static void _race_recv_coro(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    void* msg = xylem_channel_recv_timeout(ctx->ch, ctx->deadline);
    if (msg) {
        ASSERT(msg == &ctx->payload);
        ctx->got_message = 1;
    } else {
        ctx->got_timeout = 1;
    }
}

/* Drain any late-delivered message and destroy the channel from
 * inside the runtime (see teardown note above). The sender may still
 * be parked in xylem_sleep when recv timed out; this coroutine waits
 * it out, then drains so the channel is empty before destroy. */
static void _race_reaper_coro(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    /* Give a timed-out-but-pending send time to land. */
    xylem_sleep(40);
    void* leftover;
    while ((leftover = xylem_channel_recv_timeout(
                ctx->ch,
                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC))) != NULL) {
        ASSERT(leftover == &ctx->payload);
    }
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _race_main(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    _start_safety_timer();
    ctx->deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 15;
    xylem_spawn(_race_recv_coro, ctx);
    xylem_spawn(_race_sender_coro, ctx);
    xylem_spawn(_race_reaper_coro, ctx);
}

static void test_channel_timeout_race(void) {
    fprintf(stderr, "=== test_channel_timeout_race\n");
    for (int round = 0; round < TO_RACE_ROUNDS; round++) {
        _race_ctx_t ctx = {0};
        ctx.ch      = xylem_channel_create();
        ctx.payload = round;
        xylem_run(_race_main, &ctx, &_rt_opts);

        ASSERT(ctx.got_message ^ ctx.got_timeout);
    }
}

int main(void) {
    test_channel_concurrent();
    test_channel_timeout_empty();
    test_channel_timeout_deliver();
    test_channel_timeout_race();
    return 0;
}
