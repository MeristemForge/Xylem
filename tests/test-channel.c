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
#include "utils.h"

#include <stdatomic.h>
#include <stdio.h>

#define SAFETY_TIMEOUT_MS 5000

static xylem_opts_t _rt_opts = { .workers = 4 };

#define CH_SENDERS  20
#define CH_MESSAGES 10

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    atomic_int       recv_count;
    int              tested;
} _ch_ctx_t;

static void _ch_sender(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    for (int i = 0; i < CH_MESSAGES; i++) {
        xylem_channel_send(ctx->ch, &ctx->payload);
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
    _watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->ch = xylem_channel_create();
    xylem_spawn(_ch_receiver, ctx);
    for (int i = 0; i < CH_SENDERS; i++) {
        xylem_spawn(_ch_sender, ctx);
    }
}

static void test_concurrent(void) {
    fprintf(stderr, "=== test_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _ch_ctx_t ctx = {0};
        xylem_run(_test_ch_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
    }
}

/**
 * recv_timeout coverage.
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
    int              payload;
    int              tested;
} _to_ctx_t;

/**
 * An empty channel with an elapsed deadline must report a timeout
 * (NULL) rather than blocking forever.
 */
static void _to_basic_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    void* msg = xylem_channel_recv_timeout(ctx->ch, 30);
    ASSERT(msg == NULL);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _to_basic_main(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    _watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->ch = xylem_channel_create();
    xylem_spawn(_to_basic_coro, ctx);
}

static void test_timeout_empty(void) {
    fprintf(stderr, "=== test_timeout_empty\n");
    _to_ctx_t ctx = {0};
    xylem_run(_to_basic_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/**
 * A message that arrives before the deadline must be delivered, not
 * swallowed by the timeout.
 */
static void _to_sender_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    xylem_sleep(20);
    xylem_channel_send(ctx->ch, &ctx->payload);
}

static void _to_recv_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    void* msg = xylem_channel_recv_timeout(ctx->ch, 1000);
    ASSERT(msg == &ctx->payload);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _to_deliver_main(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    _watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->ch = xylem_channel_create();
    xylem_spawn(_to_recv_coro, ctx);
    xylem_spawn(_to_sender_coro, ctx);
}

static void test_timeout_deliver(void) {
    fprintf(stderr, "=== test_timeout_deliver\n");
    _to_ctx_t ctx = {0};
    xylem_run(_to_deliver_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/**
 * Stress the send/deadline overlap: drive send to land at roughly
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
 * regression.
 *
 * Teardown is synchronized deterministically with a waitgroup rather
 * than a fixed sleep: the sender done()s after send() returns and the
 * receiver wait()s before draining/destroying. This guarantees the
 * send has fully completed before the channel is freed (no race with
 * a late send touching a destroyed channel) and keeps the test fast
 * regardless of how late the send lands.
 */
#define TO_RACE_ROUNDS 200

typedef struct {
    xylem_channel_t*   ch;
    xylem_waitgroup_t* wg;
    int                payload;
    uint64_t           timeout_ms;
    uint64_t           send_at_ms;
    int                got_timeout;
    int                got_message;
} _race_ctx_t;

static void _race_sender_coro(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    /**
     * Aim the send at the deadline itself so the send wakeup and the
     * deadline timer fire in the same window.
     */
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (ctx->send_at_ms > now) {
        xylem_sleep(ctx->send_at_ms - now);
    }
    xylem_channel_send(ctx->ch, &ctx->payload);
    /**
     * Signal completion: the send has fully returned, so the receiver
     * may now safely drain and destroy the channel.
     */
    xylem_waitgroup_done(ctx->wg);
}

static void _race_recv_coro(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    void* msg = xylem_channel_recv_timeout(ctx->ch, ctx->timeout_ms);
    if (msg) {
        ASSERT(msg == &ctx->payload);
        ctx->got_message = 1;
    } else {
        ctx->got_timeout = 1;
    }

    /**
     * Single-receiver contract (see xylem-channel.h: concurrent recv
     * aborts). The receiver coroutine itself drains any late-delivered
     * message and tears the channel down -- a separate reaper calling
     * recv would be a second concurrent consumer and race q->head.
     * Wait for the sender to finish (deterministic, no timing guess):
     * once done() has fired the send has returned and the node is
     * enqueued. On a timeout the message is therefore now sitting in
     * the queue -- drain exactly that one (a plain recv returns it
     * immediately, it is already present). A late message left in the
     * queue is still a timeout from the outside, so got_message is not
     * set here.
     */
    xylem_waitgroup_wait(ctx->wg);
    if (ctx->got_timeout) {
        void* leftover = xylem_channel_recv(ctx->ch);
        ASSERT(leftover == &ctx->payload);
    }
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _race_main(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    _watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->timeout_ms = 5;
    ctx->send_at_ms =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 5;
    xylem_waitgroup_add(ctx->wg, 1);
    xylem_spawn(_race_recv_coro, ctx);
    xylem_spawn(_race_sender_coro, ctx);
}

static void test_timeout_race(void) {
    fprintf(stderr, "=== test_timeout_race\n");
    for (int round = 0; round < TO_RACE_ROUNDS; round++) {
        _race_ctx_t ctx = {0};
        ctx.ch      = xylem_channel_create();
        ctx.wg      = xylem_waitgroup_create();
        ctx.payload = round;
        xylem_run(_race_main, &ctx, &_rt_opts);

        ASSERT(ctx.got_message ^ ctx.got_timeout);
        xylem_waitgroup_destroy(ctx.wg);
    }
}

int main(void) {
    test_concurrent();
    test_timeout_empty();
    test_timeout_deliver();
    test_timeout_race();
    return 0;
}
